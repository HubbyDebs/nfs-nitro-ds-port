"""Extrae modelos 3D del juego (NSBMD) a OBJ + MTL + PNG, para abrirlos en Blender.

Uso:  .venv\\Scripts\\python.exe tools\\extract_models.py Graphics/Cars/Focus [--rom C:\\nfs] [--out CARPETA]
      (una carpeta del juego, o un archivo .nsbmd concreto)

Los modelos salen de la ROM del usuario y van a una carpeta suya, fuera del proyecto (por defecto
C:\\Users\\jj\\nfs-nitro-models\\<carpeta>). Por cada .nsbmd:
  <nombre>.obj / .mtl   la geometria: un objeto por "forma" del modelo, llamado <forma>__<huella>.
                        La huella identifica la forma en el juego: NO hay que cambiar el nombre del
                        objeto en Blender, o el juego no sabra que pieza sustituye.
  <textura>.png         las texturas, con su transparencia
  <nombre>.json         datos para volver a meter el modelo en el juego (huellas, escala, tamanos de
                        textura de cada material)
Las texturas que no van dentro del propio .nsbmd (las comparte otro archivo) se buscan en el resto
de .nsbmd de la misma carpeta.
"""
import argparse
import json
import os
import sys
import zipfile

from PIL import Image

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'models'))
import nsbmd  # noqa: E402


def export(name, data, out_dir, shared_tex):
    models, tex0 = nsbmd.load(data)
    obj_lines, mtl_lines, manifest = [], [], {'source': name, 'models': []}
    base = os.path.splitext(os.path.basename(name))[0]
    obj_lines.append('# extraido de %s (Need for Speed: Nitro, DS)' % name)
    obj_lines.append('mtllib %s.mtl' % base)
    vbase = 1
    written_tex = set()
    for m in models:
        mm = {'name': m.name, 'pos_scale': m.pos_scale, 'shapes': []}
        for node_id, mat_id, shp_id in m.draws:
            s = m.shapes[shp_id]
            mat = m.materials[mat_id] if mat_id is not None else None
            M = m.nodes[node_id][1] if node_id < len(m.nodes) else None
            tw = (mat or {}).get('width') or 1
            th = (mat or {}).get('height') or 1
            mat_name = mat['name'] if mat else 'sin_material'
            obj_lines.append('o %s__%016x' % (s['name'], s['dl_hash']))
            obj_lines.append('usemtl %s' % mat_name)
            for v in s['verts']:
                p = [c * m.pos_scale for c in v['pos']]
                if M is not None:
                    p = list((M @ [p[0], p[1], p[2], 1.0])[:3])
                c = v['col']
                obj_lines.append('v %.6f %.6f %.6f %.4f %.4f %.4f' % (p[0], p[1], p[2], c[0], c[1], c[2]))
            for v in s['verts']:
                obj_lines.append('vt %.6f %.6f' % (v['uv'][0] / tw, 1.0 - v['uv'][1] / th))
            for v in s['verts']:
                n = v['nrm']
                if M is not None:
                    n = list(M[:3, :3] @ n)
                obj_lines.append('vn %.5f %.5f %.5f' % tuple(n))
            for a, b, c in s['tris']:
                obj_lines.append('f %d/%d/%d %d/%d/%d %d/%d/%d' % tuple(x for i in (a, b, c) for x in (vbase + i,) * 3))
            vbase += len(s['verts'])
            mm['shapes'].append({'shape': s['name'], 'dl_hash': '%016x' % s['dl_hash'], 'dl_size': len(s['dl']),
                                 'material': mat_name, 'tex_width': tw, 'tex_height': th, 'node': node_id,
                                 'vertices': len(s['verts']), 'triangles': len(s['tris']),
                                 'poly_attr': '%08x' % (mat['poly_attr'] if mat else 0),
                                 'sends_normal': 0x21 in s['cmds'], 'sends_color': 0x20 in s['cmds'],
                                 'sends_texcoord': 0x22 in s['cmds'],
                                 'notes': sorted(s['notes'])})
        for mat in m.materials:
            mtl_lines.append('newmtl %s' % mat['name'])
            mtl_lines.append('Kd 1 1 1')
            tex = mat['tex']
            if tex:
                src = tex0 if tex0 and tex in tex0.textures else shared_tex.get(tex)
                png = tex + ('__' + mat['pltt'] if mat['pltt'] else '') + '.png'
                if src and png not in written_tex:
                    img = src.image(tex, mat['pltt'])
                    Image.fromarray(img, 'RGBA').save(os.path.join(out_dir, png))
                    written_tex.add(png)
                if src:
                    mtl_lines.append('map_Kd %s' % png)
                    mtl_lines.append('map_d %s' % png)
                else:
                    mtl_lines.append('# textura %s: no encontrada en esta carpeta' % tex)
            mtl_lines.append('')
        manifest['models'].append(mm)
    with open(os.path.join(out_dir, base + '.obj'), 'w') as f:
        f.write('\n'.join(obj_lines) + '\n')
    with open(os.path.join(out_dir, base + '.mtl'), 'w') as f:
        f.write('\n'.join(mtl_lines) + '\n')
    with open(os.path.join(out_dir, base + '.json'), 'w') as f:
        json.dump(manifest, f, indent=1)
    return manifest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path', help='carpeta o archivo dentro del juego, p. ej. Graphics/Cars/Focus')
    ap.add_argument('--rom', default=r'C:\nfs')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    z = zipfile.ZipFile(os.path.join(args.rom, 'data', 'vfs.zip'))
    want = args.path.replace('\\', '/').lower().rstrip('/')
    files = [i.filename for i in z.infolist() if i.filename.lower().endswith('.nsbmd') and
             (i.filename.lower() == want or os.path.dirname(i.filename).lower() == want)]
    if not files:
        sys.exit('no hay .nsbmd en ' + args.path)
    folder = os.path.dirname(files[0]) if files[0].lower() == want else args.path
    out_dir = args.out or os.path.join(r'C:\Users\jj\nfs-nitro-models', os.path.basename(folder.rstrip('/')))
    os.makedirs(out_dir, exist_ok=True)
    # texturas de toda la carpeta, por si un modelo usa las de otro archivo
    shared = {}
    for i in z.infolist():
        if i.filename.lower().endswith('.nsbmd') and os.path.dirname(i.filename).lower() == os.path.dirname(files[0]).lower():
            _, t = nsbmd.load(z.read(i.filename))
            if t:
                for name in t.textures:
                    shared.setdefault(name, t)
    for name in files:
        man = export(name, z.read(name), out_dir, shared)
        tris = sum(s['triangles'] for m in man['models'] for s in m['shapes'])
        print('%s: %d formas, %d triangulos' % (name, sum(len(m['shapes']) for m in man['models']), tris))
    print('hecho en', out_dir)


if __name__ == '__main__':
    main()
