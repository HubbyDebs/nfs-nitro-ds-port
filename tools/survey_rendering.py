"""Recorre todos los .nsbmd de vfs.zip y cuenta que efectos graficos usan de verdad:
atributos de poligono de los materiales, modos de textura, comandos de las listas de
visualizacion y comandos SBC (el "guion" de dibujado de cada modelo).

Uso: python -I tools/survey_rendering.py C:\\nfs
"""
import collections
import os
import struct
import sys
import zipfile

z = zipfile.ZipFile(os.path.join(sys.argv[1], 'data/vfs.zip'))

GX_PARAMS = {0x00: 0, 0x10: 1, 0x11: 0, 0x12: 1, 0x13: 1, 0x14: 1, 0x15: 0, 0x16: 16, 0x17: 12, 0x18: 16,
             0x19: 12, 0x1A: 9, 0x1B: 3, 0x1C: 3, 0x20: 1, 0x21: 1, 0x22: 1, 0x23: 2, 0x24: 1, 0x25: 1,
             0x26: 1, 0x27: 1, 0x28: 1, 0x29: 1, 0x2A: 1, 0x2B: 1, 0x30: 1, 0x31: 1, 0x32: 1, 0x33: 1,
             0x34: 32, 0x40: 1, 0x41: 0, 0x50: 1, 0x60: 1, 0x70: 3, 0x71: 2, 0x72: 1}
GX_NAMES = {0x10: 'MTX_MODE', 0x11: 'MTX_PUSH', 0x12: 'MTX_POP', 0x13: 'MTX_STORE', 0x14: 'MTX_RESTORE',
            0x1B: 'MTX_SCALE', 0x1C: 'MTX_TRANS', 0x20: 'COLOR', 0x21: 'NORMAL', 0x22: 'TEXCOORD',
            0x23: 'VTX_16', 0x24: 'VTX_10', 0x25: 'VTX_XY', 0x26: 'VTX_XZ', 0x27: 'VTX_YZ', 0x28: 'VTX_DIFF',
            0x29: 'POLYGON_ATTR', 0x2A: 'TEXIMAGE_PARAM', 0x2B: 'PLTT_BASE', 0x30: 'DIF_AMB', 0x31: 'SPE_EMI',
            0x32: 'LIGHT_VECTOR', 0x33: 'LIGHT_COLOR', 0x34: 'SHININESS', 0x40: 'BEGIN_VTXS', 0x41: 'END_VTXS',
            0x00: 'NOP'}
SBC_NAMES = ['NOP', 'RET', 'NODE', 'MTX', 'MAT', 'SHP', 'NODEDESC', 'BB', 'BBY', 'NODEMIX', 'CALLDL',
             'POSSCALE', 'ENVMAP', 'PRJMAP']
POLY_MODE = ['modulate', 'decal', 'toon/highlight', 'shadow']
TEXGEN = ['ninguna', 'texcoord', 'normal (envmap)', 'vertice']
PRIM = ['triangulos', 'quads', 'tira de triangulos', 'tira de quads']


def dict_entries(b, d):
    """Diccionario de NitroSystem: devuelve [(nombre, bytes del dato)]."""
    count = b[d + 1]
    ent = d + struct.unpack_from('<H', b, d + 6)[0]
    unit, ofs_name = struct.unpack_from('<HH', b, ent)
    out = []
    for i in range(count):
        data = b[ent + 4 + i * unit: ent + 4 + (i + 1) * unit]
        name = b[ent + ofs_name + i * 16: ent + ofs_name + (i + 1) * 16].rstrip(b'\0').decode('latin1')
        out.append((name, data))
    return out


def walk_dl(b, off, size, counter, prims):
    end = off + size
    while off + 4 <= end:
        cmds = b[off:off + 4]
        off += 4
        for c in cmds:
            n = GX_PARAMS.get(c)
            if n is None:
                counter['DESCONOCIDO 0x%02X' % c] += 1
                return
            counter[GX_NAMES.get(c, '0x%02X' % c)] += 1
            if c == 0x40:
                prims[PRIM[struct.unpack_from('<I', b, off)[0] & 3]] += 1
            off += n * 4


def walk_sbc(b, off, end, counter):
    while off < end:
        op = b[off] & 0x1F
        flag = b[off] >> 5
        counter[SBC_NAMES[op] if op < len(SBC_NAMES) else 'DESCONOCIDO %d' % op] += 1
        if op in (0, 0xB):
            off += 1
        elif op == 1:
            return
        elif op == 2:
            off += 2
        elif op in (3, 4, 5):
            off += 2
        elif op == 6:
            off += 4 + (flag & 1) + ((flag >> 1) & 1)
        elif op in (7, 8):
            off += 2 + (flag & 1) + ((flag >> 1) & 1)
        elif op == 9:
            off += 3 + b[off + 2] * 3
        elif op == 0xA:
            off += 9
        elif op in (0xC, 0xD):
            off += 3
        else:
            return


stats = collections.defaultdict(collections.Counter)
totals = collections.Counter()
biggest = []

for info in z.infolist():
    if not info.filename.lower().endswith('.nsbmd'):
        continue
    b = z.read(info)
    group = '/'.join(info.filename.split('/')[1:2])
    nblocks = struct.unpack_from('<H', b, 14)[0]
    for o in struct.unpack_from(f'<{nblocks}I', b, 16):
        if b[o:o + 4] != b'MDL0':
            continue
        for mname, mdata in dict_entries(b, o + 8):
            m = o + struct.unpack('<I', mdata)[0]
            size, ofs_sbc, ofs_mat, ofs_shp, ofs_evp = struct.unpack_from('<5I', b, m)
            sbc_type, scaling, texmtx, num_node, num_mat, num_shp = struct.unpack_from('<6B', b, m + 0x14)
            nvtx, npoly, ntri, nquad = struct.unpack_from('<4H', b, m + 0x24)
            totals['modelos'] += 1
            totals['vertices'] += nvtx
            totals['poligonos'] += npoly
            totals['triangulos'] += ntri
            totals['quads'] += nquad
            totals['materiales'] += num_mat
            totals['mallas'] += num_shp
            stats['nodos por modelo'][num_node if num_node < 4 else '4+'] += 1
            stats['modelos con piel (evp)'][ofs_evp != 0] += 1
            biggest.append((npoly, nvtx, info.filename))

            walk_sbc(b, m + ofs_sbc, m + ofs_mat, stats['comandos SBC'])

            mats = m + ofs_mat
            for name, data in dict_entries(b, mats + 4):
                mat = mats + struct.unpack('<I', data)[0]
                dif_amb, spe_emi, attr, attr_mask, tex, tex_mask = struct.unpack_from('<6I', b, mat + 4)
                flag = struct.unpack_from('<H', b, mat + 0x1E)[0]
                stats['luces activas (bits 0-3)'][attr & 0xF] += 1
                stats['modo de poligono'][POLY_MODE[(attr >> 4) & 3]] += 1
                stats['caras (bit6 trasera, bit7 delantera)'][(attr >> 6) & 3] += 1
                stats['niebla activada'][bool(attr & 0x8000)] += 1
                alpha = (attr >> 16) & 0x1F
                stats['alfa del material'][31 if alpha == 31 else (0 if alpha == 0 else '1-30')] += 1
                stats['generacion de coordenadas de textura'][TEXGEN[tex >> 30]] += 1
                stats['color 0 transparente'][bool(tex & (1 << 29))] += 1
                stats['repeticion/espejo (S,T,flipS,flipT)'][(tex >> 16) & 0xF] += 1
                stats['brillo especular con tabla (bit15 de spe_emi)'][bool(spe_emi & 0x8000)] += 1
                stats['color de vertice como difuso (bit15 de dif_amb)'][bool(dif_amb & 0x8000)] += 1
                stats['grupo x luces'][(group, attr & 0xF)] += 1
                if (attr >> 4) & 3 or tex >> 30 >= 2:
                    stats['grupo x efecto especial'][(group, POLY_MODE[(attr >> 4) & 3], TEXGEN[tex >> 30])] += 1
                    stats['archivos con efecto especial (ejemplos)'][info.filename.rsplit('/', 1)[1] + ' : ' + name] += 1

            shps = m + ofs_shp
            for name, data in dict_entries(b, shps):
                shp = shps + struct.unpack('<I', data)[0]
                sflag, ofs_dl, size_dl = struct.unpack_from('<3I', b, shp + 4)
                stats['mallas: normal/color/texcoord'][('N' if sflag & 1 else '-') + ('C' if sflag & 2 else '-') +
                                                         ('T' if sflag & 4 else '-')] += 1
                walk_dl(b, shp + ofs_dl, size_dl, stats['comandos GX en listas'], stats['tipos de primitiva'])

print("TOTALES:", dict(totals))
for title, counter in stats.items():
    print(f"\n== {title}")
    for k, v in sorted(counter.items(), key=lambda kv: -kv[1])[:40]:
        print(f"   {k!s:40} {v}")
print("\n== modelos con mas poligonos")
for npoly, nvtx, name in sorted(biggest, reverse=True)[:12]:
    print(f"   {npoly:6} poligonos {nvtx:6} vertices  {name}")
cars = [x for x in biggest if '/Cars/' in x[2] and x[2].endswith('lod0.nsbmd')]
print("   coches lod0: de", min(cars)[0], "a", max(cars)[0], "poligonos; media", sum(c[0] for c in cars) // len(cars))
