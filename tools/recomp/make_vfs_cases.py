"""Genera la lista de casos para port/recomp/test_vfs.c: "numero_de_archivo ruta" por cada
archivo de vfs.zip. El numero esperado se calcula con la reimplementacion en Python del
indice (la misma logica que tools/vfs_lookup.py, ya comprobada contra el ZIP).

Uso: python make_vfs_cases.py CARPETA_ROM SALIDA.txt
"""
import os
import struct
import sys
import zipfile

rom, out = sys.argv[1], sys.argv[2]
data_dir = os.path.join(rom, 'data')
n_nodes, n_files, n_names = (int(x) for x in open(os.path.join(data_dir, 'vfs.dat')).read().split(','))
vb = open(os.path.join(data_dir, 'vfs.bin'), 'rb').read()
nodes_off = 8
names_off = nodes_off + n_nodes * 8 + n_files * 4


def find(parts):
    child = struct.unpack_from('<H', vb, 4)[0]
    found = None
    for part in parts:
        i, found = child, None
        while i != 0xFFFF:
            n = struct.unpack_from('<HHHH', vb, nodes_off + i * 8)
            end = vb.index(b'\0', names_off + n[0])
            if vb[names_off + n[0]:end].decode('latin1') == part:
                found = n
                break
            i = n[1]
        if found is None:
            return None
        child = found[2]
    return found[3]


with open(out, 'w', encoding='latin1', newline='\n') as f:
    count = 0
    for info in zipfile.ZipFile(os.path.join(data_dir, 'vfs.zip')).infolist():
        path = info.filename.lower().replace('/', '\\')
        index = find(path.split('\\'))
        if index is not None:
            f.write(f"{index} {path}\n")
            count += 1
print(f"{count} casos escritos en {out}")
