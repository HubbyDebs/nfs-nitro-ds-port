"""Desensambla un tramo de arm9.bin / ITCM / overlay con capstone (herramienta de consulta).

Uso: python tools/recomp/disasm.py C:\nfs DIRECCION LONGITUD [thumb] [bloque]
"""
import struct
import sys

import capstone

from recomp import load_images

images = load_images(sys.argv[1])
addr, length = int(sys.argv[2], 16), int(sys.argv[3], 16)
thumb = len(sys.argv) > 4 and sys.argv[4] == 'thumb'
block = sys.argv[5] if len(sys.argv) > 5 else None
img = images[block] if block else next(i for n, i in images.items() if n in ('itcm', 'ram') and i.contains(addr))
md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB if thumb else capstone.CS_MODE_ARM)
a = addr
while a < addr + length:
    off = a - img.base
    insn = next(md.disasm(img.data[off:off + 4], a, 1), None)
    if insn is None:
        word = struct.unpack_from('<I', img.data, off)[0]
        print(f"{a:08x}  .word 0x{word:08x}")
        a += 4
    else:
        print(f"{a:08x}  {insn.mnemonic} {insn.op_str}")
        a += insn.size
