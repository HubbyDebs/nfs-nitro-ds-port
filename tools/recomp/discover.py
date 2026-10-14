"""Descubre funciones que Ghidra no marco: las que solo se alcanzan por un puntero
(constructores, metodos virtuales, callbacks) o por una llamada a un sitio sin funcion.

Escribe un CSV con el mismo formato que functions.csv para que recomp.py lo lea ademas.

Uso: python discover.py --rom C:\\nfs --functions port/recomp/functions.csv --out port/recomp/functions_extra.csv
                        [--missing archivo_con_direcciones]

Metodo: (1) destinos de bl/b de funciones conocidas que no caen en ninguna funcion;
(2) palabras de 32 bits fuera del codigo conocido que apuntan a la zona de codigo;
(3) direcciones que el programa pidio en marcha y no existian (--missing).
Cada candidato se recorre siguiendo sus saltos hasta los retornos; se descarta si empieza
con instrucciones invalidas.
"""
import argparse
import bisect
import csv
import os
import struct

import capstone
from capstone import arm as A

from recomp import load_images, load_functions

MD = {False: capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM),
      True: capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)}
for md in MD.values():
    md.detail = True


class Space:
    """Un bloque de codigo con sus funciones conocidas."""

    def __init__(self, img):
        self.img = img
        self.entries = {}   # direccion -> thumb
        self.starts, self.ends = [], []  # rangos ordenados de cuerpos de funcion
        self.text_end = img.base

    def add_ranges(self, ranges):
        for lo, hi in ranges:
            i = bisect.bisect_left(self.starts, lo)
            self.starts.insert(i, lo)
            self.ends.insert(i, hi)

    def inside_body(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        return i >= 0 and addr < self.ends[i]


def decode(img, addr, thumb):
    off = addr - img.base
    if off < 0 or off + (2 if thumb else 4) > len(img.data):
        return None
    for insn in MD[thumb].disasm(img.data[off:off + 4], addr, 1):
        return insn
    return None


def explore(spaces, block, entry, thumb, trusted, overlap=False, absorb=False):
    """Devuelve (rangos, llamadas) o None si el candidato no parece codigo."""
    space = spaces[block]
    img = space.img
    visited, calls, work = {}, set(), [entry]
    count = 0
    while work:
        a = work.pop()
        while True:
            if a in visited:
                break
            if a != entry and not absorb and (a in space.entries or (not overlap and space.inside_body(a))):
                break  # se mete en otra funcion conocida
            insn = decode(img, a, thumb)
            if insn is None or insn.id == 0:
                if not trusted:
                    return None  # un candidato sacado de un puntero que acaba en algo que no es codigo: datos
                break
            if not trusted and not thumb and struct.unpack_from('<I', img.data, a - img.base)[0] < 0x01000000:
                return None  # palabras pequenas (tablas de numeros) se decodifican como "andeq": son datos
            if insn.id in (A.ARM_INS_BKPT, A.ARM_INS_UDF) and not trusted and count < 6:
                return None
            visited[a] = insn.size
            count += 1
            if count > 6000:
                return None
            unconditional = insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID)
            ops = insn.operands
            target = ops[0].imm & 0xFFFFFFFF if ops and ops[0].type == A.ARM_OP_IMM else None
            if insn.id in (A.ARM_INS_BL, A.ARM_INS_BLX):
                if target is not None:
                    calls.add((target, thumb if insn.id == A.ARM_INS_BL else not thumb))
                a += insn.size
                continue
            if insn.id == A.ARM_INS_B:
                if target is not None:
                    if img.contains(target) and abs(target - entry) < 0x4000 and (absorb or not (
                            target != entry and target in space.entries)):
                        work.append(target)
                    else:
                        calls.add((target, thumb))
                if unconditional:
                    break
                a += insn.size
                continue
            if insn.id in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
                return None  # no existen en ARMv5: no es codigo de este procesador
            _, written = insn.regs_access()
            if insn.id == A.ARM_INS_BX or A.ARM_REG_PC in written:
                if unconditional:
                    break
            a += insn.size
    if not visited:
        return None
    addrs = sorted(visited)
    ranges, lo, prev_end = [], addrs[0], addrs[0] + visited[addrs[0]]
    for x in addrs[1:]:
        if x != prev_end:
            ranges.append((lo, prev_end))
            lo = x
        prev_end = x + visited[x]
    ranges.append((lo, prev_end))
    return ranges, calls


def switch_targets(img, a, thumb):
    """Si en 'a' hay un salto calculado por tabla de un patron conocido, devuelve sus destinos."""
    if thumb:
        h = img.u16(a)
        if (h & 0xFF87) != 0x4487:  # add pc, rX
            return []
        rx = (h >> 3) & 15
        if rx > 7 or a - 8 < img.base:
            return []
        same = (rx << 3) | rx
        # adds rX,rX,rX / add rX,pc / ldrh rX,[rX,#6] / lsls rX,#16 / asrs rX,#16 / add pc,rX
        if img.u16(a - 2) != (0x1400 | same) or img.u16(a - 4) != (0x0400 | same) or \
                img.u16(a - 6) != (0x88C0 | same) or img.u16(a - 8) != (0x4478 | rx):
            return []
        count = None
        for back in range(10, 40, 2):  # cmp rX, #N un poco antes
            if a - back < img.base:
                break
            c = img.u16(a - back)
            if (c & 0xF800) == 0x2800:  # cualquier registro: el indice puede venir de otro (adds rX,rY,rY)
                count = (c & 0xFF) + 1
                break
        if count is None:
            return []
        out = []
        for i in range(count):
            off = img.u16(a + 2 + 2 * i)
            if off & 0x8000:
                off -= 0x10000
            out.append(a + 4 + off)
        return out
    w = img.u32(a)
    if (w & 0xFFFFFFF0) == 0x908FF100 and a - 4 >= img.base:  # addls pc, pc, rX, lsl #2
        c = img.u32(a - 4)
        if (c & 0xFFF0FF00) == 0xE3500000 and ((c >> 16) & 15) == (w & 15):  # cmp rX, #N
            return [a + 8 + 4 * i for i in range((c & 0xFF) + 1)]
    return []


def is_fragment(img, addr, thumb):
    """El codigo en addr deshace una pila que no ha preparado? Entonces no es una funcion, sino un
    trozo (bloque de salida) de otra, aunque Ghidra lo haya marcado como funcion."""
    a = addr
    for _ in range(48):
        insn = decode(img, a, thumb)
        if insn is None or insn.id == 0:
            return False
        if insn.id == A.ARM_INS_PUSH:
            return False
        ops = insn.operands
        if insn.id == A.ARM_INS_STMDB and ops and ops[0].reg == A.ARM_REG_SP:
            return False
        if insn.id == A.ARM_INS_SUB and ops and ops[0].type == A.ARM_OP_REG and ops[0].reg == A.ARM_REG_SP:
            return False
        if insn.id == A.ARM_INS_POP or (insn.id == A.ARM_INS_LDM and ops and ops[0].reg == A.ARM_REG_SP):
            return True
        if insn.id in (A.ARM_INS_ADD, A.ARM_INS_MOV) and ops and ops[0].type == A.ARM_OP_REG and                 ops[0].reg == A.ARM_REG_SP:
            return True
        if insn.id in (A.ARM_INS_BX, A.ARM_INS_B) and insn.cc in (A.ARM_CC_AL, A.ARM_CC_INVALID):
            return False
        a += insn.size
    return False


def ends_flow(insn):
    """La instruccion corta el flujo (salto incondicional o escritura de pc sin condicion)?"""
    if insn.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
        return False
    if insn.id in (A.ARM_INS_B, A.ARM_INS_BX):
        return True
    _, written = insn.regs_access()
    return A.ARM_REG_PC in written


def extend_function(spaces, block, entry, thumb, ranges):
    """Anade a una funcion el codigo alcanzable por tablas de switch y por 'bl' internos.
    Devuelve los rangos nuevos, o None si no cambia nada."""
    space = spaces[block]
    img = space.img
    ranges = sorted(ranges)
    changed = False
    seen_targets = set()
    for _ in range(50):
        covered = lambda x: any(lo <= x < hi for lo, hi in ranges)
        span_lo, span_hi = min(lo for lo, _ in ranges), max(hi for _, hi in ranges)
        targets = []
        for lo, hi in ranges:
            a = lo
            last = None
            while a < hi:
                insn = decode(img, a, thumb)
                if insn is None or insn.id == 0:
                    a += 2 if thumb else 4
                    last = None
                    continue
                targets += switch_targets(img, a, thumb)
                imm = insn.operands[0].imm & 0xFFFFFFFF if insn.operands and                     insn.operands[0].type == A.ARM_OP_IMM else None
                if insn.id == A.ARM_INS_BL and imm is not None:
                    if span_lo <= imm < span_hi + 0x400 and imm != entry and (
                            imm not in space.entries or is_fragment(img, imm, thumb)):
                        targets.append(imm)
                # Saltos directos a codigo cercano que no esta en el cuerpo: Ghidra partio la funcion
                # en dos (o es una cola compartida). Se absorbe: duplicar codigo es inofensivo.
                if insn.id == A.ARM_INS_B and imm is not None and abs(imm - entry) < 0x3000:
                    targets.append(imm)
                last = insn
                a += insn.size
            # Si el tramo no acaba en un salto incondicional, el codigo sigue en la direccion siguiente
            if last is not None and not ends_flow(last):
                targets.append(hi)
        new = [t for t in set(targets) if t not in seen_targets and not covered(t) and img.contains(t)]
        if not new:
            break
        for t in new:
            seen_targets.add(t)
            res = explore(spaces, block, t, thumb, True, overlap=True, absorb=True)
            if res is None:
                continue
            ranges = merge_ranges(ranges + res[0])
            changed = True
    return ranges if changed else None


def merge_ranges(ranges):
    out = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--functions', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--missing', default='')
    args = ap.parse_args()

    images = load_images(args.rom)
    funcs = load_functions(args.functions, images)
    spaces = {name: Space(img) for name, img in images.items() if name != 'dtcm'}
    for fn in funcs.values():
        sp = spaces.get(fn.block)
        if sp is None:
            continue
        sp.entries[fn.addr] = fn.thumb
        sp.add_ranges(fn.ranges)
        sp.text_end = max(sp.text_end, max(hi for _, hi in fn.ranges))

    def blocks_for(src_block, target):
        """En que bloques puede estar una direccion vista desde src_block."""
        out = []
        for name in ('ram', 'itcm'):
            if spaces[name].img.base <= target < spaces[name].text_end:
                return [name]
        if src_block.startswith('ov'):
            names = [src_block]
        else:
            names = [n for n in spaces if n.startswith('ov')]
        for name in names:
            if spaces[name].img.base <= target < spaces[name].text_end:
                out.append(name)
        return out

    candidates = []  # (bloque, direccion, thumb, de_confianza)

    # (1) destinos de llamadas directas sin funcion
    for fn in list(funcs.values()):
        sp = spaces.get(fn.block)
        if sp is None:
            continue
        for lo, hi in fn.ranges:
            a = lo
            while a < hi:
                insn = decode(sp.img, a, fn.thumb)
                if insn is None or insn.id == 0:
                    a += 2 if fn.thumb else 4
                    continue
                if insn.id in (A.ARM_INS_BL, A.ARM_INS_BLX, A.ARM_INS_B) and insn.operands and \
                        insn.operands[0].type == A.ARM_OP_IMM:
                    target = insn.operands[0].imm & 0xFFFFFFFF
                    mode = (not fn.thumb) if insn.id == A.ARM_INS_BLX else fn.thumb
                    inside = any(l <= target < h for l, h in fn.ranges)
                    if not (insn.id == A.ARM_INS_B and inside):
                        for b in blocks_for(fn.block, target):
                            candidates.append((b, target, mode, True))
                a += insn.size

    # (2) punteros en los datos
    for name, img in images.items():
        sp = spaces.get(name)
        for off in range(0, len(img.data) - 3, 4):
            addr = img.base + off
            if sp is not None and sp.inside_body(addr):
                continue
            v = struct.unpack_from('<I', img.data, off)[0]
            thumb = bool(v & 1)
            target = v & ~1
            if not thumb and target & 3:
                continue
            if not (0x01FF8000 <= target < 0x02400000):
                continue
            for b in blocks_for(name if name != 'dtcm' else 'ram', target):
                candidates.append((b, target, thumb, False))

    # (3) direcciones pedidas en marcha
    if args.missing and os.path.exists(args.missing):
        for line in open(args.missing):
            line = line.strip()
            if line:
                v = int(line, 16)
                for b in blocks_for('ram', v & ~1):
                    candidates.append((b, v & ~1, bool(v & 1), 'forzado'))

    found = {}
    rejected = 0
    while candidates:
        block, addr, thumb, trusted = candidates.pop()
        sp = spaces[block]
        if addr in sp.entries:
            if sp.entries[addr] != thumb and trusted and (block, addr, thumb) not in found:
                pass  # existe en el otro modo: se deja, raro
            continue
        forced = trusted == 'forzado'  # pedida en marcha: se acepta aunque caiga dentro de otra funcion
        if sp.inside_body(addr) and not forced:
            continue  # cae en mitad de una funcion conocida: no es un inicio
        res = explore(spaces, block, addr, thumb, trusted, overlap=forced)
        if res is None:
            rejected += 1
            continue
        ranges, calls = res
        sp.entries[addr] = thumb
        if not forced:
            sp.add_ranges(ranges)
        found[(block, addr)] = (thumb, ranges, trusted)
        for target, mode in calls:
            for b in blocks_for(block, target):
                candidates.append((b, target, mode, True))

    # (4) Completar funciones a las que les falta codigo: tablas de "switch" que Ghidra no resolvio
    # y "bl" usados como salto largo dentro de la propia funcion.
    extended = {}
    everything = [(fn.block, fn.addr, fn.thumb, list(fn.ranges), fn.name) for fn in funcs.values()
                  if fn.block in spaces]
    everything += [(b, a, t, list(r), f"{'DSC' if tr else 'PTR'}_{a:08x}") for (b, a), (t, r, tr) in found.items()]
    for block, addr, thumb, ranges, name in everything:
        new_ranges = extend_function(spaces, block, addr, thumb, ranges)
        if new_ranges is not None:
            extended[(block, addr)] = (thumb, new_ranges, name)
            found.pop((block, addr), None)

    with open(args.out, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['block', 'address', 'size', 'mode', 'ranges', 'name'])
        for (block, addr), (thumb, ranges, name) in sorted(extended.items()):
            w.writerow([block, f"{addr:08x}", sum(h - l for l, h in ranges), 'thumb' if thumb else 'arm',
                        ' '.join(f"{l:08x}-{h - 1:08x}" for l, h in ranges), name])
        print(f"funciones completadas (switch / saltos largos): {len(extended)}")
        for (block, addr), (thumb, ranges, trusted) in sorted(found.items()):
            w.writerow([block, f"{addr:08x}", sum(h - l for l, h in ranges), 'thumb' if thumb else 'arm',
                        ' '.join(f"{l:08x}-{h - 1:08x}" for l, h in ranges),
                        f"{'DSC' if trusted else 'PTR'}_{addr:08x}"])
    by = {}
    for (block, addr), (thumb, ranges, trusted) in found.items():
        key = (block, 'llamada' if trusted else 'puntero', 'thumb' if thumb else 'arm')
        by[key] = by.get(key, 0) + 1
    print(f"funciones nuevas: {len(found)} (descartados {rejected} candidatos)")
    for k, v in sorted(by.items()):
        print("  ", k, v)


if __name__ == '__main__':
    main()
