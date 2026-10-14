"""Convierte un OBJ (normalmente exportado de Blender) en piezas sustitutas para el juego.

Uso: .venv\\Scripts\\python.exe tools\\models\\make_replacement.py MODELO.obj [--info CARPETA] [--out CARPETA]

Cada objeto del OBJ tiene que llamarse como la pieza que sustituye: <forma>__<huella>, por ejemplo
"polygon4__a545b3c9f799f6b1" (es el nombre que pone tools/extract_models.py; Blender lo conserva al
importar y exportar). Si en Blender un objeto acaba como "polygon4__a545b3c9f799f6b1.001" tambien vale.
Varios objetos con la misma huella se juntan en una sola pieza.

--info: carpeta con los .json que dejo extract_models.py (por defecto, la del OBJ). De ahi se saca, para
        cada huella, el tamano de su textura, la escala del modelo y si la pieza lleva luz.
--out:  donde dejar los .nmesh (por defecto C:\\Users\\jj\\nfs-nitro-models\\reemplazos, que es donde los
        busca el juego al arrancar).

Ejes: exportar de Blender con los ajustes por defecto del OBJ (adelante -Z, arriba Y), los mismos con
los que se importa. Asi el modelo queda en el mismo sitio que el original.
"""
import argparse
import glob
import json
import math
import os
import re
import struct
import sys


def load_obj(path):
    vs, vcs, vts, vns = [], [], [], []
    groups = {}  # nombre de objeto -> lista de caras [(v, vt, vn) x n]
    cur = None
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            t = line.split()
            if not t:
                continue
            if t[0] == 'v':
                vs.append(tuple(float(x) for x in t[1:4]))
                vcs.append(tuple(float(x) for x in t[4:7]) if len(t) >= 7 else None)
            elif t[0] == 'vt':
                vts.append((float(t[1]), float(t[2]) if len(t) > 2 else 0.0))
            elif t[0] == 'vn':
                vns.append(tuple(float(x) for x in t[1:4]))
            elif t[0] in ('o', 'g'):
                cur = ' '.join(t[1:])
                groups.setdefault(cur, [])
            elif t[0] == 'f':
                face = []
                for c in t[1:]:
                    parts = c.split('/')
                    idx = [int(p) if p else 0 for p in parts] + [0, 0]

                    def fix(i, n):
                        return i - 1 if i > 0 else (n + i if i < 0 else None)
                    face.append((fix(idx[0], len(vs)), fix(idx[1], len(vts)), fix(idx[2], len(vns))))
                groups.setdefault(cur, []).append(face)
    return vs, vcs, vts, vns, groups


def load_info(folder):
    info = {}
    for jf in glob.glob(os.path.join(folder, '*.json')):
        with open(jf) as f:
            man = json.load(f)
        for m in man.get('models', []):
            for s in m['shapes']:
                info[s['dl_hash']] = dict(s, pos_scale=m['pos_scale'], source=man['source'])
    return info


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('obj')
    ap.add_argument('--info', default=None)
    ap.add_argument('--out', default=os.path.join(os.environ.get('NFS_MODELS', os.path.expanduser(r'~\nfs-nitro-models')), 'reemplazos'))
    ap.add_argument('--runtime-subdiv', choices=['auto', 'si', 'no'], default='auto',
                    help='si el port debe subdividir ademas la pieza (auto: solo si no viene ya muy subdividida)')
    args = ap.parse_args()
    info = load_info(args.info or os.path.dirname(os.path.abspath(args.obj)))
    vs, vcs, vts, vns, groups = load_obj(args.obj)
    pieces = {}
    for name, faces in groups.items():
        m = re.search(r'__([0-9a-f]{16})', name or '')
        if not m:
            print('aviso: el objeto "%s" no lleva huella en el nombre: se ignora' % name)
            continue
        pieces.setdefault(m.group(1), []).extend(faces)
    os.makedirs(args.out, exist_ok=True)
    total = 0
    for h, faces in pieces.items():
        if h not in info:
            print('aviso: huella %s sin datos en los .json (falta --info?): se ignora' % h)
            continue
        si = info[h]
        # que datos manda la pieza original (se manda lo mismo: p. ej. el cristal no manda coordenadas de
        # textura, su reflejo usa las que quedaron de la pieza anterior)
        lit = si.get('sends_normal', int(si['poly_attr'], 16) & 15 != 0)
        flags = (1 if lit else 0) | (2 if si.get('sends_texcoord', True) else 0) | (4 if si.get('sends_color', not lit) else 0)
        tw, th, scale = si['tex_width'] or 1, si['tex_height'] or 1, si['pos_scale'] or 1.0
        out = []
        for face in faces:
            for k in range(1, len(face) - 1):  # abanico: cualquier poligono a triangulos
                tri = (face[0], face[k], face[k + 1])
                pts = [vs[t[0]] for t in tri]
                e1 = [pts[1][i] - pts[0][i] for i in range(3)]
                e2 = [pts[2][i] - pts[0][i] for i in range(3)]
                fn = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
                ln = math.sqrt(sum(c * c for c in fn)) or 1.0
                fn = tuple(c / ln for c in fn)
                for vi, ti, ni in tri:
                    x, y, z = (round(c / scale * 4096) for c in vs[vi])
                    u, v = vts[ti] if ti is not None and ti < len(vts) else (0.0, 0.0)
                    s16 = clamp(round(u * tw * 16), -32768, 32767)
                    t16 = clamp(round((1.0 - v) * th * 16), -32768, 32767)
                    n = vns[ni] if ni is not None and ni < len(vns) else fn
                    nl = math.sqrt(sum(c * c for c in n)) or 1.0
                    nx, ny, nz = (clamp(round(c / nl * 511), -511, 511) for c in n)
                    c = vcs[vi] or (1.0, 1.0, 1.0)
                    col = clamp(round(c[0] * 31), 0, 31) | clamp(round(c[1] * 31), 0, 31) << 5 | clamp(round(c[2] * 31), 0, 31) << 10
                    out.append(struct.pack('<iiihhhhhH', x, y, z, s16, t16, nx, ny, nz, col))
        # Si la pieza ya viene muy subdividida (mas del triple de triangulos que la original), el port no
        # la subdivide otra vez; si no, se le aplica la misma subdivision que al resto (NFS_TESS).
        if args.runtime_subdiv == 'no' or (args.runtime_subdiv == 'auto' and len(out) // 3 > 3 * si['triangles']):
            flags |= 8
        path = os.path.join(args.out, h + '.nmesh')
        with open(path, 'wb') as f:
            f.write(struct.pack('<4sII', b'NMS1', flags, len(out)))
            f.write(b''.join(out))
        total += 1
        print('%s (%s, %s): %d triangulos (el original tenia %d) -> %s' % (
            h, si['source'], si['shape'], len(out) // 3, si['triangles'], path))
    if not total:
        sys.exit('no se ha generado ninguna pieza')
    print('hecho: %d piezas. El juego las carga al arrancar.' % total)


if __name__ == '__main__':
    main()
