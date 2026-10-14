"""Busca cadenas de texto en arm9.bin y en los overlays y da su direccion de RAM.

Uso: python -I tools/arm9_strings.py C:\\nfs [filtro1 filtro2 ...]
Sin filtros imprime un resumen; con filtros, las cadenas que contienen alguno (sin distinguir mayusculas).
"""
import re
import struct
import sys
import os

root = sys.argv[1]
filters = [f.lower() for f in sys.argv[2:]]

images = [('arm9', 0x02000000, os.path.join(root, 'arm9.bin'))]
y9 = open(os.path.join(root, 'y9.bin'), 'rb').read()
for i in range(len(y9) // 32):
    ov_id, ram = struct.unpack_from('<II', y9, i * 32)
    images.append((f'ov{ov_id}', ram, os.path.join(root, f'overlay/overlay_{ov_id:04d}.bin')))

for name, base, path in images:
    data = open(path, 'rb').read()
    found = [(base + m.start(), m.group().decode('latin1')) for m in re.finditer(rb'[\x20-\x7e]{5,}', data)]
    if not filters:
        print(f"{name}: base 0x{base:08X}, {len(data)} bytes, {len(found)} cadenas")
        continue
    for addr, s in found:
        if any(f in s.lower() for f in filters):
            print(f"{name} 0x{addr:08X}  {s}")
