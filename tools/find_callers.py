"""Busca en arm9.bin y en los overlays quien llama o apunta a una direccion.

Uso: python -I tools/find_callers.py C:\\nfs 0x02085324 [mas direcciones]
Encuentra: llamadas ARM (BL/BLX), llamadas Thumb (BL/BLX) y punteros de 32 bits (con y sin el bit Thumb).
Es una busqueda a ciegas por bytes: puede dar falsos positivos, hay que confirmarlos en Ghidra.
"""
import os
import struct
import sys

root = sys.argv[1]
targets = [int(a, 16) & ~1 for a in sys.argv[2:]]

images = [('arm9', 0x02000000, os.path.join(root, 'arm9.bin'))]
y9 = open(os.path.join(root, 'y9.bin'), 'rb').read()
for i in range(len(y9) // 32):
    ov_id, ram = struct.unpack_from('<II', y9, i * 32)
    images.append((f'ov{ov_id}', ram, os.path.join(root, f'overlay/overlay_{ov_id:04d}.bin')))

for name, base, path in images:
    data = open(path, 'rb').read()
    for off in range(0, len(data) - 3, 2):
        pc = base + off
        if off % 4 == 0:
            w = struct.unpack_from('<I', data, off)[0]
            if (w & ~1) in targets:
                print(f"{name} 0x{pc:08X}  puntero -> 0x{w:08X}")
            top = w >> 24
            if top == 0xEB or top in (0xFA, 0xFB):
                imm = w & 0xFFFFFF
                if imm & 0x800000:
                    imm -= 0x1000000
                dest = pc + 8 + imm * 4 + (2 if top == 0xFB else 0)
                if dest in targets:
                    print(f"{name} 0x{pc:08X}  ARM {'blx' if top != 0xEB else 'bl'} -> 0x{dest:08X}")
        h1, h2 = struct.unpack_from('<HH', data, off)
        if (h1 & 0xF800) == 0xF000 and (h2 & 0xE800) == 0xE800:
            imm = ((h1 & 0x7FF) << 12) | ((h2 & 0x7FF) << 1)
            if imm & 0x400000:
                imm -= 0x800000
            dest = pc + 4 + imm
            if not h2 & 0x1000:
                dest &= ~3
            if dest in targets:
                print(f"{name} 0x{pc:08X}  Thumb {'bl' if h2 & 0x1000 else 'blx'} -> 0x{dest:08X}")
