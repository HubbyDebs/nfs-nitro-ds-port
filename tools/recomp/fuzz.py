"""Prueba del traductor instruccion a instruccion contra un emulador de referencia (Unicorn).

Coge las instrucciones distintas que aparecen en el juego (sin saltos ni llamadas), las pone
una detras de otra en una imagen de prueba, las traduce a C con recomp.py y las ejecuta con
registros y memoria aleatorios, tanto en Unicorn como en el C traducido. Compara registros,
banderas y escrituras en memoria.

Uso: .venv\\Scripts\\python.exe tools/recomp/fuzz.py --rom C:\\nfs [--arm 30000] [--thumb 30000]
Todo lo que genera va a la carpeta de compilacion (contiene instrucciones del juego).
"""
import argparse
import collections
import os
import random
import struct
import subprocess
import sys

import capstone
from capstone import arm as A
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_WRITE
from unicorn import arm_const as U

from recomp import Func, Image, Translator, load_functions, load_images

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(os.path.dirname(HERE))
RT = os.path.join(PROJECT, 'port', 'recomp')
CLANG = r'C:\Users\jj\nfs-nitro-tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang.exe'

BASE = 0x02000000
WIN_LO = 0x021F8000   # ventana de memoria de datos para las pruebas
WIN_SIZE = 0x10000
PTR_LO = 0x021FF000   # los registros "puntero" apuntan al centro de la ventana

SKIP_IDS = {A.ARM_INS_B, A.ARM_INS_BL, A.ARM_INS_BLX, A.ARM_INS_BX, A.ARM_INS_SVC, A.ARM_INS_BKPT,
            A.ARM_INS_MSR, A.ARM_INS_MRS, A.ARM_INS_MCR, A.ARM_INS_MRC, A.ARM_INS_CDP, A.ARM_INS_LDC,
            A.ARM_INS_STC, A.ARM_INS_UDF, A.ARM_INS_CBZ, A.ARM_INS_CBNZ, A.ARM_INS_IT}
UC_REGS = [U.UC_ARM_REG_R0 + i for i in range(13)] + [U.UC_ARM_REG_SP, U.UC_ARM_REG_LR]


def acceptable(insn):
    if insn.id == 0 or insn.id in SKIP_IDS:
        return False
    _, written = insn.regs_access()
    if A.ARM_REG_PC in written:
        return False
    for op in insn.operands:
        if op.type == A.ARM_OP_REG and op.reg == A.ARM_REG_PC and insn.id not in (A.ARM_INS_ADD, A.ARM_INS_ADR):
            return False  # pc como operando general: raro y con alineaciones especiales
    return True


def collect(rom, csvs, limits, rng):
    images = load_images(rom)
    funcs = {}
    for path in csvs:
        for key, fn in load_functions(path, images).items():
            funcs.setdefault(key, fn)
    md = {t: capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if t else capstone.CS_MODE_ARM)
          for t in (False, True)}
    for m in md.values():
        m.detail = True
    uniq = {False: {}, True: {}}
    for fn in funcs.values():
        img = images[fn.block]
        for lo, hi in fn.ranges:
            data = img.data[lo - img.base:hi - img.base]
            for insn in md[fn.thumb].disasm(data, lo):
                raw = bytes(insn.bytes)
                if raw in uniq[fn.thumb] or (fn.thumb and len(raw) != 2):
                    continue
                if acceptable(insn):
                    uniq[fn.thumb][raw] = f"{insn.mnemonic} {insn.op_str}"
    out = {}
    for thumb in (False, True):
        items = sorted(uniq[thumb].items())
        rng.shuffle(items)
        out[thumb] = items[:limits[thumb]]
        print(f"{'thumb' if thumb else 'arm'}: {len(uniq[thumb])} instrucciones distintas, se prueban {len(out[thumb])}")
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=r'C:\nfs')
    ap.add_argument('--build', default=r'C:\Users\jj\nfs-nitro-build\fuzz')
    ap.add_argument('--arm', type=int, default=30000)
    ap.add_argument('--thumb', type=int, default=30000)
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    rng = random.Random(args.seed)
    os.makedirs(args.build, exist_ok=True)

    csvs = [os.path.join(RT, 'functions_extra.csv'), os.path.join(RT, 'functions.csv')]
    chosen = collect(args.rom, csvs, {False: args.arm, True: args.thumb}, rng)

    # Imagen de prueba: cada instruccion es una "funcion" de una sola instruccion
    image = bytearray()
    tests = []  # (direccion, thumb, texto)
    for thumb in (False, True):
        while len(image) % 4:
            image += b'\x00'
        for raw, text in chosen[thumb]:
            tests.append((BASE + len(image), thumb, text, len(raw)))
            image += raw
    image += b'\x00' * 16
    img = Image('ram', BASE, bytes(image))
    funcs = {}
    for addr, thumb, text, size in tests:
        funcs[('ram', addr)] = Func('ram', addr, thumb, [(addr, addr + size)], 'T')
    tr = Translator({'ram': img, 'itcm': Image('itcm', 0x01FF8000, b'')}, funcs)
    bodies = []
    for fn in funcs.values():
        code, _ = tr.translate(fn)
        bodies.append(code)
    with open(os.path.join(args.build, 'fuzz_funcs.c'), 'w', newline='\n') as f:
        f.write('#include "rt.h"\n')
        for fn in funcs.values():
            f.write(f"void {fn.cname}(Cpu *c);\n")
        f.write('\n'.join(bodies))
        f.write('\nconst FnEntry g_fn_table[] = {\n')
        for fn in sorted(funcs.values(), key=lambda x: x.addr | x.thumb):
            f.write(f'  {{0x{fn.addr | fn.thumb:08x}u, "ram", {fn.cname}}},\n')
        f.write(f'}};\nconst int g_fn_count = {len(funcs)};\n')
    print("traps del traductor:", tr.trap_kinds)

    window = bytes(rng.getrandbits(8) for _ in range(WIN_SIZE))
    open(os.path.join(args.build, 'fuzz_image.bin'), 'wb').write(bytes(image))
    open(os.path.join(args.build, 'fuzz_window.bin'), 'wb').write(window)

    # Referencia: Unicorn
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(BASE, 0x400000)
    uc.mem_write(BASE, bytes(image))
    uc.mem_write(WIN_LO, window)
    writes = []
    uc.hook_add(UC_HOOK_MEM_WRITE, lambda u, acc, addr, size, value, ud: writes.append((addr, size, value & 0xFFFFFFFF)))

    def vector(kind):
        if kind == 0:
            return [rng.getrandbits(32) for _ in range(15)]
        if kind == 1:
            return [PTR_LO + 4 * rng.randrange(0x400) for _ in range(15)]
        return [PTR_LO + 4 * rng.randrange(0x400) if rng.random() < 0.5 else 4 * rng.randrange(0x40)
                for _ in range(15)]

    records = bytearray()
    count = skipped = 0
    for addr, thumb, text, size in tests:
        ok_for_insn = 0
        for kind in (0, 0, 1, 2, 2, 2):
            regs = vector(kind)
            flags = rng.getrandbits(4)
            # Primero el CPSR (modo sistema): si no, sp y lr se escribirian en el banco de otro modo
            uc.reg_write(U.UC_ARM_REG_CPSR, (flags << 28) | 0x1F | (0x20 if thumb else 0))
            for reg, value in zip(UC_REGS, regs):
                uc.reg_write(reg, value)
            del writes[:]
            try:
                uc.emu_start(addr | thumb, addr + size, count=1)
            except UcError:
                for a, s, _ in writes:
                    if WIN_LO <= a < WIN_LO + WIN_SIZE:
                        uc.mem_write(a, window[a - WIN_LO:a - WIN_LO + s])
                continue
            out = [uc.reg_read(reg) for reg in UC_REGS]
            out_flags = uc.reg_read(U.UC_ARM_REG_CPSR) >> 28
            good = all(WIN_LO <= a and a + s <= WIN_LO + WIN_SIZE for a, s, _ in writes) and len(writes) <= 16
            for a, s, _ in writes:
                if WIN_LO <= a < WIN_LO + WIN_SIZE:
                    uc.mem_write(a, window[a - WIN_LO:a - WIN_LO + s])
            if not good:
                continue
            records += struct.pack('<I15sI', addr | thumb, b'', 0)[:4]
            records += struct.pack('<15II15III', *regs, flags, *out, out_flags, len(writes))
            for a, s, v in writes:
                records += struct.pack('<III', a, s, v)
            count += 1
            ok_for_insn += 1
        if not ok_for_insn:
            skipped += 1
    open(os.path.join(args.build, 'fuzz_tests.bin'), 'wb').write(bytes(records))
    print(f"casos de prueba: {count} (instrucciones sin ningun caso valido: {skipped})")

    exe = os.path.join(args.build, 'fuzz.exe')
    subprocess.check_call([CLANG, '-O1', '-w', '-I' + RT, os.path.join(RT, 'fuzz_main.c'),
                           os.path.join(RT, 'interp.c'), os.path.join(args.build, 'fuzz_funcs.c'), '-o', exe])
    text_of = {addr | thumb: (text, thumb) for addr, thumb, text, size in tests}
    for title, extra in (("C TRADUCIDO", []), ("INTERPRETE DE RESERVA", ['interp'])):
        print(f"\n######## {title}")
        report(subprocess.run([exe, args.build] + extra, capture_output=True, text=True), text_of)


def report(res, text_of):
    groups = collections.defaultdict(list)
    for line in res.stdout.splitlines():
        if line.startswith('MAL '):
            _, key, what = line.split(' ', 2)
            text, thumb = text_of[int(key, 16)]
            groups[('thumb ' if thumb else 'arm ') + text.split()[0]].append((text, what))
        else:
            print(line)
    for name, items in sorted(groups.items(), key=lambda kv: -len(kv[1])):
        print(f"\n== {name}: {len(items)} fallos")
        seen = set()
        for text, what in items:
            if text not in seen and len(seen) < 6:
                seen.add(text)
                print(f"   {text:40} {what}")


if __name__ == '__main__':
    main()
