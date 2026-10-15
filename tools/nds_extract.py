"""Extrae una ROM de Nintendo DS (.nds) en la misma forma que ndstool, para no tener que instalarlo.

Uso: python tools/nds_extract.py juego.nds CARPETA_DESTINO

Escribe: header.bin, arm9.bin (con su pie de 12 bytes si lo tiene, como ndstool), arm7.bin, y9.bin, y7.bin,
banner.bin, overlay/overlay_NNNN.bin y data/ con el arbol de archivos de la ROM (tablas FNT y FAT).
Equivale a: ndstool -x juego.nds -9 arm9.bin -7 arm7.bin -y9 y9.bin -y7 y7.bin -d data -y overlay -t banner.bin -h header.bin
"""
import os
import struct
import sys


def u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def write(path, data):
    os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data)


def extract(rom_path, out):
    with open(rom_path, 'rb') as f:
        rom = f.read()
    if len(rom) < 0x200:
        raise ValueError('el archivo es demasiado pequeno para ser una ROM de DS')
    write(os.path.join(out, 'header.bin'), rom[:0x200])
    arm9_off, arm9_size = u32(rom, 0x20), u32(rom, 0x2C)
    arm9 = rom[arm9_off:arm9_off + arm9_size]
    if u32(rom, arm9_off + arm9_size) == 0xDEC00621:  # pie de nitrocode: ndstool lo deja pegado al arm9.bin
        arm9 = rom[arm9_off:arm9_off + arm9_size + 12]
    write(os.path.join(out, 'arm9.bin'), arm9)
    arm7_off, arm7_size = u32(rom, 0x30), u32(rom, 0x3C)
    write(os.path.join(out, 'arm7.bin'), rom[arm7_off:arm7_off + arm7_size])
    fnt_off, fat_off = u32(rom, 0x40), u32(rom, 0x48)
    y9_off, y9_size, y7_off, y7_size = u32(rom, 0x50), u32(rom, 0x54), u32(rom, 0x58), u32(rom, 0x5C)
    write(os.path.join(out, 'y9.bin'), rom[y9_off:y9_off + y9_size])
    write(os.path.join(out, 'y7.bin'), rom[y7_off:y7_off + y7_size])
    banner_off = u32(rom, 0x68)
    if banner_off:
        version = u16(rom, banner_off)
        size = {1: 0x840, 2: 0x940, 3: 0xA40}.get(version, 0x840)
        if version == 0x103:
            size = 0x23C0
        write(os.path.join(out, 'banner.bin'), rom[banner_off:banner_off + size])

    def file_data(fid):
        start, end = u32(rom, fat_off + fid * 8), u32(rom, fat_off + fid * 8 + 4)
        return rom[start:end]

    overlay_ids = set()
    for k in range(y9_size // 32):  # tabla de overlays del ARM9: id en +0, archivo en +0x18
        e = y9_off + k * 32
        ov_id, fid = u32(rom, e), u32(rom, e + 0x18)
        overlay_ids.add(fid)
        write(os.path.join(out, 'overlay', 'overlay_%04d.bin' % ov_id), file_data(fid))
    for k in range(y7_size // 32):
        overlay_ids.add(u32(rom, y7_off + k * 32 + 0x18))

    count = 0

    def walk(dir_id, path):
        nonlocal count
        entry = fnt_off + (dir_id & 0xFFF) * 8
        sub, fid = fnt_off + u32(rom, entry), u16(rom, entry + 4)
        while True:
            n = rom[sub]
            if n == 0:
                break
            name = rom[sub + 1:sub + 1 + (n & 0x7F)].decode('latin-1')
            sub += 1 + (n & 0x7F)
            if n & 0x80:  # subcarpeta
                sub_id = u16(rom, sub)
                sub += 2
                os.makedirs(os.path.join(path, name), exist_ok=True)
                walk(sub_id, os.path.join(path, name))
            else:
                write(os.path.join(path, name), file_data(fid))
                fid += 1
                count += 1

    data_dir = os.path.join(out, 'data')
    os.makedirs(data_dir, exist_ok=True)
    walk(0xF000, data_dir)
    return count


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    n = extract(sys.argv[1], sys.argv[2])
    print('extraida: %d archivos en data\\' % n)


if __name__ == '__main__':
    main()
