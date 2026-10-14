"""Recompilador estatico ARM9 (ARMv5TE, modos ARM y Thumb) -> C para NFS Nitro (DS).

Lee la lista de funciones exportada de Ghidra (functions.csv) y los binarios de la ROM
extraida, y escribe un archivo C con una funcion C por cada funcion del juego.

El C generado es codigo del juego traducido: NO va en el repositorio. Se escribe en una
carpeta de compilacion aparte.

Uso:
  python recomp.py --rom C:\\nfs --functions port/recomp/functions.csv --out BUILD_DIR
                   [--roots 0x0209d758,...]   (solo esas funciones y lo que llaman)
"""
import argparse
import csv
import os
import re
import struct

COND = ['c->fz', '!c->fz', 'c->fc', '!c->fc', 'c->fn', '!c->fn', 'c->fv', '!c->fv',
        '(c->fc && !c->fz)', '(!c->fc || c->fz)', '(c->fn == c->fv)', '(c->fn != c->fv)',
        '(!c->fz && c->fn == c->fv)', '(c->fz || c->fn != c->fv)']

# Operaciones de datos de ARM
AND, EOR, SUB, RSB, ADD, ADC, SBC, RSC, TST, TEQ, CMP, CMN, ORR, MOV, BIC, MVN = range(16)
LOGICAL = {AND: 'a & b', EOR: 'a ^ b', TST: 'a & b', TEQ: 'a ^ b', ORR: 'a | b', MOV: 'b', BIC: 'a & ~b',
           MVN: '~b'}
ARITH_FLAGS = {SUB: 'add_f(c, a, ~b, 1)', RSB: 'add_f(c, b, ~a, 1)', ADD: 'add_f(c, a, b, 0)',
               ADC: 'add_f(c, a, b, c->fc)', SBC: 'add_f(c, a, ~b, c->fc)', RSC: 'add_f(c, b, ~a, c->fc)',
               CMP: 'add_f(c, a, ~b, 1)', CMN: 'add_f(c, a, b, 0)'}
ARITH = {SUB: 'a - b', RSB: 'b - a', ADD: 'a + b', ADC: 'a + b + c->fc', SBC: 'a + ~b + c->fc',
         RSC: 'b + ~a + c->fc'}


class Image:
    def __init__(self, name, base, data):
        self.name, self.base, self.data = name, base, data

    def contains(self, addr):
        return self.base <= addr < self.base + len(self.data)

    def u16(self, addr):
        return struct.unpack_from('<H', self.data, addr - self.base)[0]

    def u32(self, addr):
        return struct.unpack_from('<I', self.data, addr - self.base)[0]


class Func:
    def __init__(self, block, addr, thumb, ranges, name):
        self.block, self.addr, self.thumb, self.ranges, self.name = block, addr, thumb, ranges, name
        self.cname = f"{block}_{addr:08x}"
        self.callname = self.cname  # nombre al que llaman los demas (cambia si hay sustituto nativo)


def load_images(rom):
    arm9 = open(os.path.join(rom, 'arm9.bin'), 'rb').read()
    images = {'ram': Image('ram', 0x02000000, arm9)}
    # Parametros del modulo: estan justo antes de la marca 0xDEC00621 del arranque
    mp = arm9.index(struct.pack('<I', 0xDEC00621)) - 0x1C
    al_start, al_end, al_data = struct.unpack_from('<3I', arm9, mp)
    src = al_data - 0x02000000
    for i, off in enumerate(range(al_start - 0x02000000, al_end - 0x02000000, 12)):
        addr, size, bss = struct.unpack_from('<3I', arm9, off)
        images['itcm' if i == 0 else 'dtcm'] = Image('itcm' if i == 0 else 'dtcm', addr, arm9[src:src + size])
        src += size
    y9 = open(os.path.join(rom, 'y9.bin'), 'rb').read()
    for i in range(len(y9) // 32):
        ov_id, ram = struct.unpack_from('<II', y9, i * 32)
        data = open(os.path.join(rom, f'overlay/overlay_{ov_id:04d}.bin'), 'rb').read()
        images[f'ov{ov_id}'] = Image(f'ov{ov_id}', ram, data)
    return images


def load_functions(path, images):
    funcs = {}
    with open(path, newline='', encoding='utf-8') as f:
        for row in csv.DictReader(f):
            if row['block'] not in images:
                continue
            ranges = []
            for part in row['ranges'].split():
                lo, hi = part.split('-')
                ranges.append((int(lo, 16), int(hi, 16) + 1))
            fn = Func(row['block'], int(row['address'], 16), row['mode'] == 'thumb', ranges, row['name'])
            funcs[(fn.block, fn.addr)] = fn
    return funcs


class Translator:
    def __init__(self, images, funcs):
        self.images, self.funcs = images, funcs
        self.abi_check = False
        self.setjmp_funcs = {}  # (bloque, direccion) -> funcion de la capa que da el jmp_buf del PC
        self.stats = {'instrucciones': 0, 'traps': 0, 'llamadas indirectas fijas': 0}
        self.trap_kinds = {}

    # ---- resolucion de destinos -------------------------------------------------
    def resolve(self, block, target, thumb):
        for name in ('ram', 'itcm', block):
            if self.images[name].contains(target):
                fn = self.funcs.get((name, target))
                if fn is not None and fn.thumb == thumb:
                    return fn
                return None
        return None

    # ---- traduccion de una funcion ----------------------------------------------
    def translate(self, fn):
        self.fn = fn
        self.thumb = fn.thumb
        self.img = self.images[fn.block]
        self.callees = set()
        self.need_dispatch = False

        insns = []
        for lo, hi in fn.ranges:
            a = lo
            while a < hi:
                if self.thumb:
                    h = self.img.u16(a)
                    size = 2
                    if (h >> 11) == 0x1E and a + 2 < self.img.base + len(self.img.data):
                        if (self.img.u16(a + 2) >> 11) in (0x1F, 0x1D):
                            size = 4
                else:
                    size = 4
                insns.append((a, size))
                a += size
        self.insn_set = {a for a, _ in insns}

        # Hay "bl" hacia dentro de la propia funcion? (salto largo o subrutina local del compilador)
        # Entonces un retorno puede volver a una direccion de esta misma funcion: los retornos
        # pasan por el despachador, que sabe saltar a cualquier instruccion de la funcion.
        self.local_returns = False
        for a, size in insns:
            if self.thumb and size == 4:
                h, h2 = self.img.u16(a), self.img.u16(a + 2)
                off = ((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
                if off & 0x400000:
                    off -= 0x800000
                target = a + 4 + off
                if h2 & 0x1000 and target in self.insn_set and target != fn.addr:
                    self.local_returns = True
            elif not self.thumb:
                w = self.img.u32(a)
                if (w & 0x0F000000) == 0x0B000000 and (w >> 28) != 15:
                    off = w & 0xFFFFFF
                    if off & 0x800000:
                        off -= 0x1000000
                    target = a + 8 + off * 4
                    if target in self.insn_set and target != fn.addr:
                        self.local_returns = True

        body = []
        self.prev_call_setup = False
        for a, size in insns:
            self.terminated = False
            self.cur_addr = a
            self.stats['instrucciones'] += 1
            if self.thumb:
                cond, stmts = self.lift_thumb(a, size)
            else:
                cond, stmts = self.lift_arm(a)
            text = ' '.join(stmts)
            if cond is not None:
                text = f"if ({COND[cond]}) {{ {text} }}"
                self.terminated = False
            nxt = a + size
            if not self.terminated and nxt not in self.insn_set:
                # Se sale del cuerpo conocido de la funcion: seguir en la direccion siguiente
                text += f" call_indirect(c, 0x{nxt | self.thumb:08x}u); return;"
            body.append(f"L_{a:08x}: {{ {text} }}")

        own = fn.cname if fn.callname == fn.cname else fn.cname + '_orig'
        out = []
        if self.abi_check:
            # Modo depuracion: una envoltura comprueba que la funcion devuelve r4-r11 y sp como estaban
            out += [f"static void {own}_b(Cpu *c);",
                    f"void {own}(Cpu *c) {{ uint32_t s[9]; memcpy(s, c->r + 4, 32); s[8] = c->r[13]; {own}_b(c); "
                    f"for (int i = 0; i < 8; i++) if (c->r[4 + i] != s[i]) abi_violation(c, 0x{fn.addr | fn.thumb:08x}u, 4 + i, s[i]); "
                    f"if (c->r[13] != s[8]) abi_violation(c, 0x{fn.addr | fn.thumb:08x}u, 13, s[8]); }}"]
            own = 'static ' + own + '_b'
            out.append(f"void {own[7:]}(Cpu *c) {{ /* {fn.name} {'thumb' if fn.thumb else 'arm'} */".replace('void', 'static void', 1))
        else:
            out.append(f"void {own}(Cpu *c) {{ /* {fn.name} {'thumb' if fn.thumb else 'arm'} */")
        out += [
               "  uint32_t *r = c->r; uint32_t ret = r[14]; uint32_t pc = 0; (void)ret; (void)pc;",
               f"  g_ring[g_ring_i++ & 0xFF] = 0x{fn.addr | fn.thumb:08x}u;"]
        if insns and insns[0][0] != fn.addr:
            # La funcion tiene codigo en direcciones anteriores a su entrada: hay que empezar por la entrada
            if fn.addr not in self.insn_set:
                raise SystemExit(f"la entrada de {fn.cname} no esta en su propio cuerpo")
            out.append(f"  goto L_{fn.addr:08x};")
        out += ['  ' + line for line in body]
        if self.need_dispatch:
            cases = [f"    case 0x{a:08x}u: goto L_{a:08x};" for a, _ in insns]
            out.append("  L_dispatch_ret: (void)0;")  # viene de un retorno: si no es local, se vuelve
            out.append("  switch (pc & ~1u) {")
            out += cases
            out.append("  }")
            out.append("  return;")
            out.append("  L_dispatch:")  # salto calculado
            out.append("  if (((pc ^ ret) & ~1u) == 0) return;")
            out.append("  switch (pc & ~1u) {")
            out += cases
            out.append("  }")
            out.append("  call_indirect(c, pc); return;")
        out.append("}")
        return '\n'.join(out), self.callees

    # ---- piezas comunes ------------------------------------------------------------
    def recent_pops(self, a):
        """Registros sacados de la pila por 'pop' en las 3 instrucciones Thumb anteriores."""
        regs = set()
        for back in (2, 4, 6):
            if a - back in self.insn_set:
                h = self.img.u16(a - back)
                if (h & 0xFE00) == 0xBC00:
                    regs |= {i for i in range(8) if (h >> i) & 1}
        return regs

    def reg(self, n, a):
        if n == 15:
            return f"0x{a + (4 if self.thumb else 8):08x}u"
        return f"r[{n}]"

    def trap(self, a, what):
        self.stats['traps'] += 1
        self.trap_kinds[what] = self.trap_kinds.get(what, 0) + 1
        self.terminated = True
        return [f'trap(c, 0x{a:08x}u, "{what}"); return;']

    def call(self, target, thumb):
        fn = self.resolve(self.fn.block, target, thumb)
        if fn is not None:
            self.callees.add(fn)
            if (fn.block, fn.addr) in self.setjmp_funcs:
                # OS_SaveContext: el punto de reanudacion tiene que quedar en ESTA funcion (ver threads.c)
                slot = self.setjmp_funcs[(fn.block, fn.addr)]
                return [f"if (setjmp(*{slot}(c, r[0])) == 0) {{ {fn.callname}(c); }}"]
            return [f"{fn.callname}(c);"]
        self.stats['llamadas indirectas fijas'] += 1
        return [f"call_indirect(c, 0x{target | thumb:08x}u);"]

    def branch(self, a, target, thumb, link, ret_addr=None):
        if link:
            if thumb == self.thumb and target in self.insn_set and target != self.fn.addr:
                # "bl" a mitad de la propia funcion: el compilador original lo usa como salto largo,
                # no como llamada. Se pone lr igual que haria el procesador y se salta.
                self.terminated = True
                check = f"if (!--g_budget) watchdog(c, 0x{a:08x}u); " if target <= a else ""
                return [f"r[14] = 0x{ret_addr:08x}u; {check}goto L_{target:08x};"]
            return [f"r[14] = 0x{ret_addr:08x}u;"] + self.call(target, thumb)
        self.terminated = True
        if thumb == self.thumb and target in self.insn_set:
            if target <= a:  # salto hacia atras: punto de control para vigilar bucles e interrupciones
                return [f"if (!--g_budget) watchdog(c, 0x{a:08x}u); goto L_{target:08x};"]
            return [f"goto L_{target:08x};"]
        return self.call(target, thumb) + ["return;"]

    def jump_ret(self, expr):
        """Salto a un valor que normalmente es la direccion de retorno."""
        if self.prev_call_setup:
            return [f"call_indirect(c, {expr});"]
        self.terminated = True
        if self.local_returns:
            # Puede ser el retorno de una subrutina local: si el destino es de esta funcion, se salta alli
            self.need_dispatch = True
            return [f"pc = {expr}; goto L_dispatch_ret;"]
        # Retorno. Si el destino no es la direccion con la que se entro, hay dos casos:
        #  - es el inicio de una funcion: salto disfrazado ("push destino; pop {pc}" de los thunks de C++);
        #  - no lo es: se llego a esta funcion por un salto desde otra (cola compartida) y lr de entrada
        #    ya no valia; la pila de llamadas de C si es la correcta, asi que basta con volver.
        return [f"{{ uint32_t t = {expr}; if (((t ^ ret) & ~1u) != 0 && rt_is_function(t)) call_indirect(c, t); return; }}"]

    def jump_reg(self, expr):
        """bx a un registro que no es lr: salto indirecto a otra funcion (o retorno, si coincide)."""
        if self.prev_call_setup:
            return [f"call_indirect(c, {expr});"]
        nxt = self.cur_addr + (2 if self.thumb else 4)
        if nxt in self.insn_set:
            # Si lr apunta a la instruccion siguiente, es una llamada (lr cargado a mano antes del salto)
            return [f"{{ uint32_t t = {expr}; if ((r[14] & ~1u) == 0x{nxt:08x}u) {{ call_indirect(c, t); }} else {{ "
                    f"if (((t ^ ret) & ~1u) != 0) call_indirect(c, t); return; }} }}"]
        self.terminated = True
        return [f"{{ uint32_t t = {expr}; if (((t ^ ret) & ~1u) == 0) return; call_indirect(c, t); return; }}"]

    def jump_computed(self, expr):
        """Salto calculado (tablas de switch, etc.)."""
        if self.prev_call_setup:
            return [f"call_indirect(c, {expr});"]
        self.terminated = True
        self.need_dispatch = True
        return [f"pc = {expr}; goto L_dispatch;"]

    def imm_shift(self, typ, amt, rm):
        s = [f"uint32_t sv = {rm};"]
        if typ == 0:
            if amt == 0:
                return s, "sv", None
            return s, f"(sv << {amt})", f"((sv >> {32 - amt}) & 1)"
        if typ == 1:
            if amt == 0:
                return s, "0u", "(sv >> 31)"
            return s, f"(sv >> {amt})", f"((sv >> {amt - 1}) & 1)"
        if typ == 2:
            if amt == 0:
                return s, "(uint32_t)((int32_t)sv >> 31)", "(sv >> 31)"
            return s, f"(uint32_t)((int32_t)sv >> {amt})", f"((sv >> {amt - 1}) & 1)"
        if amt == 0:
            return s, "((c->fc << 31) | (sv >> 1))", "(sv & 1)"
        return s, f"((sv >> {amt}) | (sv << {32 - amt}))", f"((sv >> {amt - 1}) & 1)"

    def reg_shift(self, typ, rm, rs):
        return [f"uint32_t sc = c->fc; uint32_t sb = sh_reg({typ}, {rm}, {rs}, &sc);"], "sb", "sc"

    def dp(self, a, op, flags, rd, aexpr, pre, bexpr, carry, ret_like=False):
        test = op in (TST, TEQ, CMP, CMN)
        if rd == 15 and flags and not test:
            return self.trap(a, "retorno de excepcion (S con pc)")
        out = list(pre)
        out.append(f"uint32_t a = {aexpr or '0'}, b = {bexpr}; (void)a;")
        if op in LOGICAL:
            out.append(f"uint32_t res = {LOGICAL[op]};")
            if flags or test:
                out.append("c->fn = res >> 31; c->fz = (res == 0);" + (f" c->fc = {carry};" if carry else ""))
        elif flags or test:
            out.append(f"uint32_t res = {ARITH_FLAGS[op]};")
        else:
            out.append(f"uint32_t res = {ARITH[op]};")
        if test:
            out.append("(void)res;")
            return out
        if rd != 15:
            out.append(f"r[{rd}] = res;")
            return out
        expr = "(res | 1)" if self.thumb else "res"
        return out + (self.jump_ret(expr) if ret_like else self.jump_computed(expr))

    # ---- ARM -------------------------------------------------------------------------
    def lift_arm(self, a):
        w = self.img.u32(a)
        cond = w >> 28
        call_setup = self.prev_call_setup
        self.prev_call_setup = False
        if cond == 15:
            if (w & 0x0E000000) == 0x0A000000:  # blx inmediato
                off = w & 0xFFFFFF
                if off & 0x800000:
                    off -= 0x1000000
                target = a + 8 + off * 4 + ((w >> 23) & 2)
                return None, self.branch(a, target, True, True, a + 4)
            return None, self.trap(a, "arm cond=1111")
        self.prev_call_setup = call_setup
        stmts = self.lift_arm_body(a, w)
        self.prev_call_setup = (w & 0x0FFFFFFF) == 0x01A0E00F  # mov lr, pc
        return (None if cond == 14 else cond), stmts

    def lift_arm_body(self, a, w):
        R = lambda n: self.reg(n, a)
        top = (w >> 25) & 7

        if (w & 0x0FFFFFF0) == 0x012FFF10:  # bx
            return self.jump_ret(R(14)) if (w & 15) == 14 else self.jump_reg(R(w & 15))
        if (w & 0x0FFFFFF0) == 0x012FFF30:  # blx registro
            self.stats['llamadas indirectas fijas'] += 0
            return [f"{{ uint32_t t = {R(w & 15)}; r[14] = 0x{a + 4:08x}u; call_indirect(c, t); }}"]
        if (w & 0x0FFF0FF0) == 0x016F0F10:  # clz
            return [f"r[{(w >> 12) & 15}] = clz32({R(w & 15)});"]
        if (w & 0x0FBF0FFF) == 0x010F0000:  # mrs
            return [f"r[{(w >> 12) & 15}] = cpsr_read(c, {(w >> 22) & 1});"]
        if (w & 0x0FB0FFF0) == 0x0120F000:  # msr registro
            return [f"cpsr_write(c, {R(w & 15)}, {(w >> 16) & 15}, {(w >> 22) & 1});"]
        if (w & 0x0FB0F000) == 0x0320F000:  # msr inmediato
            rot = ((w >> 8) & 15) * 2
            imm = w & 0xFF
            val = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
            return [f"cpsr_write(c, 0x{val:08x}u, {(w >> 16) & 15}, {(w >> 22) & 1});"]
        if (w & 0x0FC000F0) == 0x00000090:  # mul / mla
            rd, rn, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
            acc = f" + {R(rn)}" if w & (1 << 21) else ""
            out = [f"uint32_t res = {R(rm)} * {R(rs)}{acc}; r[{rd}] = res;"]
            if w & (1 << 20):
                out.append("c->fn = res >> 31; c->fz = (res == 0);")
            return out
        if (w & 0x0F8000F0) == 0x00800090:  # multiplicaciones largas
            rdhi, rdlo, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
            if w & (1 << 22):
                prod = f"(uint64_t)((int64_t)(int32_t){R(rm)} * (int64_t)(int32_t){R(rs)})"
            else:
                prod = f"((uint64_t){R(rm)} * (uint64_t){R(rs)})"
            acc = f" + (((uint64_t)r[{rdhi}] << 32) | r[{rdlo}])" if w & (1 << 21) else ""
            out = [f"uint64_t res = {prod}{acc}; r[{rdlo}] = (uint32_t)res; r[{rdhi}] = (uint32_t)(res >> 32);"]
            if w & (1 << 20):
                out.append("c->fn = (uint32_t)(res >> 63); c->fz = (res == 0);")
            return out
        if (w & 0x0FB00FF0) == 0x01000090:  # swp
            rn, rd, rm = (w >> 16) & 15, (w >> 12) & 15, w & 15
            if w & (1 << 22):
                return [f"{{ uint32_t ea = {R(rn)}; uint32_t t = mem_r8(ea); mem_w8(ea, {R(rm)}); r[{rd}] = t; }}"]
            return [f"{{ uint32_t ea = {R(rn)}; uint32_t t = mem_r32(ea); mem_w32(ea, {R(rm)}); r[{rd}] = t; }}"]
        if (w & 0x0F900090) == 0x01000080:  # multiplicaciones DSP (smulxy, smlaxy...)
            return self.trap(a, "multiplicacion DSP")
        if (w & 0x0F9000F0) == 0x01000050:  # qadd/qsub
            return self.trap(a, "aritmetica saturada")
        if (w & 0x0FF000F0) == 0x01200070:
            return self.trap(a, "bkpt")
        if (w & 0x0E000090) == 0x00000090 and (w & 0x60):  # medias palabras y bytes con signo
            return self.arm_halfword(a, w)

        if top in (0, 1):  # operaciones de datos
            op, flags = (w >> 21) & 15, (w >> 20) & 1
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            if w & (1 << 25):
                rot = ((w >> 8) & 15) * 2
                imm = w & 0xFF
                val = ((imm >> rot) | (imm << (32 - rot))) & 0xFFFFFFFF if rot else imm
                pre, b, carry = [], f"0x{val:08x}u", (str(val >> 31) if rot else None)
            elif w & 0x10:
                pre, b, carry = self.reg_shift((w >> 5) & 3, R(w & 15), R((w >> 8) & 15))
            else:
                pre, b, carry = self.imm_shift((w >> 5) & 3, (w >> 7) & 31, R(w & 15))
            aexpr = None if op in (MOV, MVN) else R(rn)
            ret_like = op == MOV and (w & 0x0FFF) == 0x00E  # mov pc, lr
            return self.dp(a, op, flags, rd, aexpr, pre, b, carry, ret_like)

        if top in (2, 3):  # ldr / str / ldrb / strb
            P, U, B, W, L = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
            rn, rd = (w >> 16) & 15, (w >> 12) & 15
            out = []
            if top == 3:
                if w & 0x10:
                    return self.trap(a, "arm indefinida")
                pre, off, _ = self.imm_shift((w >> 5) & 3, (w >> 7) & 31, R(w & 15))
                out += pre
            else:
                off = f"{w & 0xFFF}u"
            return out + self.load_store(a, L, 'b' if B else 'w', rn, rd, off, P, U, (not P) or W)

        if top == 4:  # ldm / stm
            P, U, S, W, L = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
            if S:
                return self.trap(a, "ldm/stm con registros de usuario")
            return self.block_transfer(a, L, (w >> 16) & 15, [i for i in range(16) if (w >> i) & 1], P, U, W)

        if top == 5:  # b / bl
            off = w & 0xFFFFFF
            if off & 0x800000:
                off -= 0x1000000
            target = a + 8 + off * 4
            return self.branch(a, target, False, bool(w & (1 << 24)), a + 4)

        if (w & 0x0F000010) == 0x0E000010:  # mrc / mcr
            crn, rd, cp, op2, crm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, (w >> 5) & 7, w & 15
            if cp != 15:
                return self.trap(a, "coprocesador distinto de cp15")
            if w & (1 << 20):
                return [f"r[{rd}] = cp15_read(c, {crn}, {crm}, {op2});"]
            return [f"cp15_write(c, {crn}, {crm}, {op2}, {R(rd)});"]
        if (w & 0x0F000000) == 0x0F000000:
            return [f"swi_handler(c, {(w >> 16) & 0xFF});"]
        return self.trap(a, "arm no reconocida")

    def arm_halfword(self, a, w):
        P, U, I, W, L = (w >> 24) & 1, (w >> 23) & 1, (w >> 22) & 1, (w >> 21) & 1, (w >> 20) & 1
        rn, rd, sh = (w >> 16) & 15, (w >> 12) & 15, (w >> 5) & 3
        off = f"{((w >> 4) & 0xF0) | (w & 15)}u" if I else self.reg(w & 15, a)
        if L:
            kind = {1: 'h', 2: 'sb', 3: 'sh'}[sh]
        elif sh == 1:
            kind = 'h'
        else:
            return self.trap(a, "ldrd/strd")
        return self.load_store(a, L, kind, rn, rd, off, P, U, (not P) or W)

    def load_store(self, a, load, kind, rn, rd, off, P, U, writeback):
        out = [f"uint32_t base = {self.reg(rn, a)}, offs = {off};",
               f"uint32_t wb = base {'+' if U else '-'} offs, ea = {'wb' if P else 'base'}; (void)wb;"]
        if load:
            read = {'w': 'mem_r32(ea)', 'b': 'mem_r8(ea)', 'h': 'mem_r16(ea)',
                    'sb': '(uint32_t)(int32_t)(int8_t)mem_r8(ea)',
                    'sh': '(uint32_t)(int32_t)(int16_t)mem_r16(ea)'}[kind]
            out.append(f"uint32_t val = {read};")
            if writeback and rn != rd:
                out.append(f"r[{rn}] = wb;")
            if rd == 15:
                return out + (self.jump_ret("val") if rn == 13 else self.jump_computed("val"))
            out.append(f"r[{rd}] = val;")
        else:
            src = f"0x{a + 12:08x}u" if rd == 15 else f"r[{rd}]"
            out.append({'w': f"mem_w32(ea, {src});", 'b': f"mem_w8(ea, {src});", 'h': f"mem_w16(ea, {src});"}[kind])
            if writeback:
                out.append(f"r[{rn}] = wb;")
        return out

    def block_transfer(self, a, load, rn, regs, P, U, W):
        n = len(regs)
        if n == 0:
            return self.trap(a, "ldm/stm con lista vacia")
        if U:
            start, final = ("base + 4" if P else "base"), f"base + {4 * n}"
        else:
            start, final = (f"base - {4 * n}" if P else f"base - {4 * n} + 4"), f"base - {4 * n}"
        out = [f"uint32_t base = r[{rn}]; uint32_t ad = {start};"]
        if load:
            if W and rn not in regs:
                out.append(f"r[{rn}] = {final};")
            for i, rg in enumerate(regs):
                if rg == 15:
                    out.append(f"uint32_t tpc = mem_r32(ad + {4 * i});")
                else:
                    out.append(f"r[{rg}] = mem_r32(ad + {4 * i});")
            if 15 in regs:
                out += self.jump_ret("tpc")
        else:
            for i, rg in enumerate(regs):
                src = f"0x{a + 12:08x}u" if rg == 15 else f"r[{rg}]"
                out.append(f"mem_w32(ad + {4 * i}, {src});")
            if W:
                out.append(f"r[{rn}] = {final};")
        return out

    # ---- Thumb ------------------------------------------------------------------------
    def lift_thumb(self, a, size):
        self.prev_call_setup = False
        h = self.img.u16(a)
        R = lambda n: self.reg(n, a)

        if size == 4:  # bl / blx de dos mitades
            h2 = self.img.u16(a + 2)
            off = ((h & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
            if off & 0x400000:
                off -= 0x800000
            target = a + 4 + off
            to_thumb = bool(h2 & 0x1000)
            if not to_thumb:
                target &= ~3
            return None, self.branch(a, target, to_thumb, True, (a + 4) | 1)

        top5 = h >> 11
        if top5 < 3:  # desplazamiento por inmediato
            rd, rs, amt = h & 7, (h >> 3) & 7, (h >> 6) & 31
            pre, b, carry = self.imm_shift(top5, amt, R(rs))
            return None, self.dp(a, MOV, True, rd, None, pre, b, carry)
        if top5 == 3:  # add/sub de 3 operandos
            rd, rs, rn = h & 7, (h >> 3) & 7, (h >> 6) & 7
            b = f"{rn}u" if h & 0x400 else R(rn)
            return None, self.dp(a, SUB if h & 0x200 else ADD, True, rd, R(rs), [], b, None)
        if (h >> 13) == 1:  # mov/cmp/add/sub con inmediato de 8 bits
            rd, imm = (h >> 8) & 7, h & 0xFF
            op = [MOV, CMP, ADD, SUB][(h >> 11) & 3]
            return None, self.dp(a, op, True, rd, None if op == MOV else R(rd), [], f"{imm}u", None)
        if (h >> 10) == 0x10:  # operaciones entre registros bajos
            rd, rs, op = h & 7, (h >> 3) & 7, (h >> 6) & 15
            if op in (2, 3, 4, 7):
                typ = {2: 0, 3: 1, 4: 2, 7: 3}[op]
                pre, b, carry = self.reg_shift(typ, R(rd), R(rs))
                return None, self.dp(a, MOV, True, rd, None, pre, b, carry)
            if op == 9:  # neg
                return None, self.dp(a, RSB, True, rd, R(rs), [], "0u", None)
            if op == 13:  # mul
                return None, [f"uint32_t res = r[{rd}] * r[{rs}]; r[{rd}] = res; c->fn = res >> 31; c->fz = (res == 0);"]
            arm_op = {0: AND, 1: EOR, 5: ADC, 6: SBC, 8: TST, 10: CMP, 11: CMN, 12: ORR, 14: BIC, 15: MVN}[op]
            return None, self.dp(a, arm_op, True, rd, None if arm_op == MVN else R(rd), [], R(rs), None)
        if (h >> 10) == 0x11:  # registros altos / bx
            op = (h >> 8) & 3
            rd = (h & 7) | ((h >> 4) & 8)
            rs = (h >> 3) & 15
            if op == 3:
                if h & 0x80:  # blx registro
                    return None, [f"{{ uint32_t t = {R(rs)}; r[14] = 0x{(a + 2) | 1:08x}u; call_indirect(c, t); }}"]
                if rs == 15:  # bx pc: pasa a modo ARM en la direccion alineada siguiente
                    return None, self.branch(a, (a + 4) & ~3, False, False)
                if rs == 14 or rs in self.recent_pops(a):
                    return None, self.jump_ret(R(rs))  # bx lr, o "pop {rX}; bx rX" (retorno al estilo antiguo)
                return None, self.jump_reg(R(rs))
            if op == 0:
                return None, self.dp(a, ADD, False, rd, R(rd), [], R(rs), None)
            if op == 1:
                return None, self.dp(a, CMP, True, rd, R(rd), [], R(rs), None)
            return None, self.dp(a, MOV, False, rd, None, [], R(rs), None, ret_like=(rd == 15 and rs == 14))
        if top5 == 9:  # ldr rd, [pc, #imm]
            lit = ((a + 4) & ~3) + (h & 0xFF) * 4
            return None, [f"r[{(h >> 8) & 7}] = mem_r32(0x{lit:08x}u);"]
        if (h >> 12) == 5:  # ldr/str con registro de desplazamiento
            rd, rb, ro = h & 7, (h >> 3) & 7, (h >> 6) & 7
            sel = (h >> 9) & 7
            load, kind = [(0, 'w'), (0, 'h'), (0, 'b'), (1, 'sb'), (1, 'w'), (1, 'h'), (1, 'b'), (1, 'sh')][sel]
            return None, self.load_store(a, load, kind, rb, rd, R(ro), 1, 1, False)
        if (h >> 13) == 3:  # ldr/str con inmediato
            rd, rb, imm = h & 7, (h >> 3) & 7, (h >> 6) & 31
            byte = h & 0x1000
            return None, self.load_store(a, (h >> 11) & 1, 'b' if byte else 'w', rb, rd,
                                         f"{imm if byte else imm * 4}u", 1, 1, False)
        if (h >> 12) == 8:  # ldrh/strh con inmediato
            rd, rb, imm = h & 7, (h >> 3) & 7, (h >> 6) & 31
            return None, self.load_store(a, (h >> 11) & 1, 'h', rb, rd, f"{imm * 2}u", 1, 1, False)
        if (h >> 12) == 9:  # ldr/str relativo a sp
            return None, self.load_store(a, (h >> 11) & 1, 'w', 13, (h >> 8) & 7, f"{(h & 0xFF) * 4}u", 1, 1, False)
        if (h >> 12) == 10:  # add rd, pc/sp, #imm
            rd, imm = (h >> 8) & 7, (h & 0xFF) * 4
            if h & 0x800:
                return None, [f"r[{rd}] = r[13] + {imm}u;"]
            return None, [f"r[{rd}] = 0x{(((a + 4) & ~3) + imm) & 0xFFFFFFFF:08x}u;"]
        if (h >> 8) == 0xB0:  # add sp, #imm
            imm = (h & 0x7F) * 4
            return None, [f"r[13] {'-' if h & 0x80 else '+'}= {imm}u;"]
        if (h & 0xF600) == 0xB400:  # push / pop
            regs = [i for i in range(8) if (h >> i) & 1]
            if h & 0x800:
                if h & 0x100:
                    regs.append(15)
                return None, self.block_transfer(a, 1, 13, regs, 0, 1, 1)
            if h & 0x100:
                regs.append(14)
            return None, self.block_transfer(a, 0, 13, regs, 1, 0, 1)
        if (h >> 12) == 12:  # stmia / ldmia
            rb = (h >> 8) & 7
            regs = [i for i in range(8) if (h >> i) & 1]
            return None, self.block_transfer(a, (h >> 11) & 1, rb, regs, 0, 1, 1)
        if (h >> 12) == 13:  # salto condicional / swi
            cond = (h >> 8) & 15
            if cond == 15:
                return None, [f"swi_handler(c, {h & 0xFF});"]
            if cond == 14:
                return None, self.trap(a, "thumb indefinida")
            off = h & 0xFF
            if off & 0x80:
                off -= 0x100
            return cond, self.branch(a, a + 4 + off * 2, True, False)
        if top5 == 0x1C:  # salto incondicional
            off = h & 0x7FF
            if off & 0x400:
                off -= 0x800
            return None, self.branch(a, a + 4 + off * 2, True, False)
        if (h >> 8) == 0xBE:
            return None, self.trap(a, "bkpt")
        return None, self.trap(a, "thumb no reconocida")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--functions', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--roots', default='')
    ap.add_argument('--hle', default='', help='direcciones con sustituto nativo hle_<bloque>_<dir>')
    ap.add_argument('--trace', default='', help='direcciones cuyas llamadas se imprimen (argumentos y resultado)')
    ap.add_argument('--abi-check', action='store_true', help='depuracion: comprobar registros conservados')
    ap.add_argument('--setjmp', default='', help='direcciones de OS_SaveContext (ver port/recomp/threads.c)')
    args = ap.parse_args()

    images = load_images(args.rom)
    funcs = {}
    for path in args.functions.split(','):  # el primero manda (Ghidra); los demas solo anaden
        for key, fn in load_functions(path, images).items():
            funcs.setdefault(key, fn)
    tr = Translator(images, funcs)

    def parse(item):
        block, _, addr = item.rpartition(':')
        return funcs[(block or 'ram', int(addr, 16))]

    for item in filter(None, args.hle.split(',')):
        fn = parse(item)
        fn.callname = 'hle_' + fn.cname
    tr.abi_check = args.abi_check
    traced = []
    for item in filter(None, args.trace.split(',')):
        fn = parse(item)
        fn.callname = 'trace_' + fn.cname
        traced.append(fn)
    for item in filter(None, args.setjmp.split(',')):
        addr, _, slot = item.partition('=')
        fn = parse(addr)
        tr.setjmp_funcs[(fn.block, fn.addr)] = slot or 'rt_ctx_slot'

    todo = [parse(i) for i in args.roots.split(',')] if args.roots else list(funcs.values())
    done, bodies = {}, {}
    while todo:
        fn = todo.pop()
        if (fn.block, fn.addr) in done:
            continue
        code, callees = tr.translate(fn)
        done[(fn.block, fn.addr)] = fn
        bodies[(fn.block, fn.addr)] = code
        if args.roots:
            todo.extend(callees)

    os.makedirs(args.out, exist_ok=True)
    nl = chr(10)
    banner = '/* GENERADO por recomp.py. Codigo del juego traducido: no compartir. */' + nl
    ordered = sorted(done.values(), key=lambda f: (f.block, f.addr))

    def write_if_changed(name, text):
        path = os.path.join(args.out, name)
        if os.path.exists(path):
            with open(path, encoding='utf-8', newline='') as f:
                if f.read() == text:
                    return
        with open(path, 'w', encoding='utf-8', newline='') as f:
            f.write(text)

    header = [banner + '#include "rt.h"']
    for fn in ordered:
        header.append(f"void {fn.cname}(Cpu *c);")
        if fn.callname != fn.cname:
            header.append(f"void {fn.cname}_orig(Cpu *c); void {fn.callname}(Cpu *c);")
    write_if_changed('recomp_funcs.h', nl.join(header) + nl)

    # Un archivo por bloque y tramo de 32 KB de direcciones: asi, al anadir funciones,
    # solo cambian (y se recompilan) los archivos afectados.
    chunks = {}
    for fn in ordered:
        chunks.setdefault(f'rc_{fn.block}_{fn.addr >> 15:04x}.c', []).append(bodies[(fn.block, fn.addr)])
    for name, chunk in chunks.items():
        write_if_changed(name, banner + '#include "recomp_funcs.h"' + nl + nl + (nl + nl).join(chunk) + nl)
    for old in os.listdir(args.out):
        if re.fullmatch('rc_.*[.](c|o)', old) and old != 'rc_table.c' and old != 'rc_table.o'                 and os.path.splitext(old)[0] + '.c' not in chunks:
            os.remove(os.path.join(args.out, old))

    table = [banner + '#include <stdio.h>' + nl + '#include "recomp_funcs.h"' + nl]
    for fn in traced:
        table.append(f'void {fn.callname}(Cpu *c) {{ uint32_t lr = c->r[14]; printf("[traza] {fn.name} {fn.addr:08x}(%08x, %08x, %08x, %08x) desde %08x%c", c->r[0], c->r[1], c->r[2], c->r[3], lr, 10); '
                     f'{fn.cname}_orig(c); printf("[traza] {fn.name} {fn.addr:08x} -> %08x%c", c->r[0], 10); }}')
    table.append('const FnEntry g_fn_table[] = {')
    for fn in sorted(ordered, key=lambda f: ((f.addr | f.thumb), f.block)):
        table.append(f'  {{0x{fn.addr | fn.thumb:08x}u, "{fn.block}", {fn.callname}}},')
    table.append('};' + nl + f'const int g_fn_count = {len(ordered)};')
    write_if_changed('rc_table.c', nl.join(table) + nl)

    print(f"funciones traducidas: {len(done)} en {len(chunks)} archivos")
    for k, v in tr.stats.items():
        print(f"{k}: {v}")
    for k, v in sorted(tr.trap_kinds.items(), key=lambda kv: -kv[1]):
        print(f"  trap '{k}': {v}")


if __name__ == '__main__':
    main()
