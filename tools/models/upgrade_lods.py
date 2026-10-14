"""Hace que los coches lejanos se vean con el modelo de carrera (lod1) en vez de con los modelos
simplificados (lod2, lod3), que en la DS apenas ocupaban unos pixeles pero a alta resolucion se ven toscos
(ruedas como cajas, traseras con bandas oscuras).

Uso: .venv\\Scripts\\python.exe tools\\models\\upgrade_lods.py [--rom C:\\nfs] [--out CARPETA]

Para cada coche de Graphics/Cars: cada pieza de lod2/lod3 (y wheel_lod2/3) se sustituye por las piezas de
lod1 (wheel_lod1) que usan el MISMO material: comparten textura, asi que la textura, la pintura y la
posicion que pone el juego valen igual. Se escriben como piezas sustitutas (.nmesh, mismo formato que
make_replacement.py) en la carpeta de reemplazos, que el juego carga al arrancar. Las piezas de lod1 que no
tienen equivalente en lod2 (las luces de freno) no se dibujan de lejos.
"""
import argparse
import os
import struct
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nsbmd  # noqa: E402


def clamp(v, lo, hi):
    return lo if v < lo else hi if v > hi else v


def shape_info(model):
    """forma -> (material, tamano de textura, nodo)"""
    out = {}
    for node, mat, shp in model.draws:
        m = model.materials[mat] if mat is not None else None
        out[shp] = (m['name'] if m else None, (m['width'] or 1, m['height'] or 1) if m else (1, 1), node)
    return out


def build_mesh(src_model, src_shapes, tgt_model, tgt_shape, tgt_tex):
    """Triangulos de las formas de origen, en las unidades de la forma de destino"""
    cmds = tgt_shape['cmds']
    flags = (1 if 0x21 in cmds else 0) | (2 if 0x22 in cmds else 0) | (4 if 0x20 in cmds else 0)
    out = []
    for shp, (tw, th) in src_shapes:
        k = src_model.pos_scale / (tgt_model.pos_scale or 1.0)
        su, sv = tgt_tex[0] / tw, tgt_tex[1] / th
        for tri in shp['tris']:
            for vi in tri:
                v = shp['verts'][vi]
                x, y, z = (round(c * k * 4096) for c in v['pos'])
                s16 = clamp(round(v['uv'][0] * su * 16), -32768, 32767)
                t16 = clamp(round(v['uv'][1] * sv * 16), -32768, 32767)
                nx, ny, nz = (clamp(round(c * 511), -511, 511) for c in v['nrm'])
                c = v['col']
                col = clamp(round(c[0] * 31), 0, 31) | clamp(round(c[1] * 31), 0, 31) << 5 | clamp(round(c[2] * 31), 0, 31) << 10
                out.append(struct.pack('<iiihhhhhH', x, y, z, s16, t16, nx, ny, nz, col))
    return flags, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=r'C:\nfs')
    ap.add_argument('--out', default=os.path.join(os.environ.get('NFS_MODELS', os.path.expanduser(r'~\nfs-nitro-models')), 'reemplazos'))
    args = ap.parse_args()
    z = zipfile.ZipFile(os.path.join(args.rom, 'data', 'vfs.zip'))
    names = {i.filename.lower(): i.filename for i in z.infolist()}
    cars = sorted({n.split('/')[2] for n in names if n.startswith('graphics/cars/') and n.count('/') == 3 and n.endswith('.nsbmd')})
    os.makedirs(args.out, exist_ok=True)
    written = skipped = 0
    pending = {}  # huella -> contenido; si dos coches tienen la misma pieza de lejos con distinto sustituto, no se toca
    for car in cars:
        for best, lows in (('lod1', ('lod2', 'lod3')), ('wheel_lod1', ('wheel_lod2', 'wheel_lod3'))):
            key = 'graphics/cars/%s/%s.nsbmd' % (car, best)
            if key not in names:
                continue
            src_models, _ = nsbmd.load(z.read(names[key]))
            src = src_models[0]
            if any(abs(M[:3, :3] - __import__('numpy').eye(3)).max() > 1e-6 or abs(M[:3, 3]).max() > 1e-6 for _, M in src.nodes):
                skipped += 1  # con nodos movidos: no se toca (no pasa en los coches vistos)
                continue
            sinfo = shape_info(src)
            by_mat = {}
            for si, (mat, tex, node) in sinfo.items():
                by_mat.setdefault(mat, []).append((src.shapes[si], tex))
            for low in lows:
                lk = 'graphics/cars/%s/%s.nsbmd' % (car, low)
                if lk not in names:
                    continue
                tgt_models, _ = nsbmd.load(z.read(names[lk]))
                tgt = tgt_models[0]
                for ti, (mat, tex, node) in shape_info(tgt).items():
                    shp = tgt.shapes[ti]
                    if mat not in by_mat or len(shp['dl']) == 0:
                        skipped += 1
                        continue
                    flags, out = build_mesh(src, by_mat[mat], tgt, shp, tex)
                    if not out:
                        continue
                    data = struct.pack('<4sII', b'NMS1', flags, len(out)) + b''.join(out)
                    h = shp['dl_hash']
                    if h in pending and pending[h] != data:
                        pending[h] = None  # ambigua
                    elif h not in pending:
                        pending[h] = data
    # Luces de freno y de marcha atras (materiales blight / rlight): la misma geometria, marcada para que el port
    # las dibuje como luz sumada (en la DS son un brillo de pocos pixeles; a alta resolucion tapaban el piloto)
    lights = 0
    for car in cars:
        for lod in ('lod0', 'lod1', 'lod2', 'lod3'):
            key = 'graphics/cars/%s/%s.nsbmd' % (car, lod)
            if key not in names:
                continue
            m = nsbmd.load(z.read(names[key]))[0][0]
            for si, (mat, tex, node) in shape_info(m).items():
                if mat not in ('blight', 'rlight'):
                    continue
                shp = m.shapes[si]
                h = shp['dl_hash']
                if h in pending:
                    continue
                flags, out = build_mesh(m, [(shp, tex)], m, shp, tex)
                flags |= 16 if mat == 'blight' else 32
                pending[h] = struct.pack('<4sII', b'NMS1', flags, len(out)) + b''.join(out)
                lights += 1
    print('luces de freno y marcha atras marcadas: %d' % lights)
    ambiguous = 0
    for h, data in pending.items():
        path = os.path.join(args.out, '%016x.nmesh' % h)
        if data is None:
            ambiguous += 1
            continue
        with open(path, 'wb') as f:
            f.write(data)
        written += 1
    print('coches: %d, piezas lejanas sustituidas por las de lod1: %d, sin equivalente: %d, compartidas por varios coches '
          '(se dejan): %d -> %s' % (len(cars), written, skipped, ambiguous, args.out))


if __name__ == '__main__':
    main()
