"""Reimplementa en Python la busqueda de archivos del juego (VfsIndex_FindFile, 0x0209D758)
y comprueba que el indice vfs.bin lleva al mismo archivo que el ZIP.

Uso: python -I tools/vfs_lookup.py C:\\nfs [ruta ...]
Sin rutas, comprueba TODOS los archivos del ZIP.
"""
import os
import struct
import sys
import zipfile

root = sys.argv[1]
data_dir = os.path.join(root, 'data')

n_nodes, n_files, n_names = (int(x) for x in open(os.path.join(data_dir, 'vfs.dat')).read().split(','))
vb = open(os.path.join(data_dir, 'vfs.bin'), 'rb').read()

HEADER = 8
nodes_off = HEADER
files_off = nodes_off + n_nodes * 8
names_off = files_off + n_files * 4
assert names_off + n_names == len(vb), "el tamano de vfs.bin no cuadra con vfs.dat"

NONE = 0xFFFF


def node(i):
    # nombre, hermano siguiente, primer hijo, numero de archivo
    return struct.unpack_from('<HHHH', vb, nodes_off + i * 8)


def name(off):
    end = vb.index(b'\0', names_off + off)
    return vb[names_off + off:end].decode('latin1')


def find(path):
    """Devuelve (numero de archivo, valor de la tabla de archivos) o None."""
    parts = path.lower().replace('/', '\\').split('\\')
    child = struct.unpack_from('<H', vb, 4)[0]  # primer hijo de la raiz, en la cabecera
    found = None
    for part in parts:
        i = child
        found = None
        while i != NONE:
            n = node(i)
            if name(n[0]) == part:
                found = n
                break
            i = n[1]
        if found is None:
            return None
        child = found[2]
    return found[3], struct.unpack_from('<I', vb, files_off + found[3] * 4)[0]


z = zipfile.ZipFile(os.path.join(data_dir, 'vfs.zip'))
infos = z.infolist()

# Posicion de cada entrada dentro del directorio central del ZIP (46 bytes fijos + nombre + extra + comentario)
cd_pos = {}
pos = 0
for i in infos:
    cd_pos[i.filename] = pos
    pos += 46 + len(i.filename.encode('cp437')) + len(i.extra) + len(i.comment)

paths = sys.argv[2:] or [i.filename for i in infos]
ok = bad = missing = 0
for p in paths:
    r = find(p)
    if r is None:
        missing += 1
        print(f"NO ENCONTRADO en vfs.bin: {p}")
        continue
    if r[1] == cd_pos.get(p):
        ok += 1
    else:
        bad += 1
        if bad <= 10:
            print(f"DISTINTO: {p}: vfs.bin dice {r[1]}, el ZIP dice {cd_pos.get(p)}")
    if len(paths) <= 10:
        print(f"{p}: archivo n. {r[0]}, posicion en el directorio del ZIP {r[1]}")
print(f"coinciden: {ok}, distintos: {bad}, no encontrados: {missing}, de {len(paths)}")
