"""Mejora los modelos de todos los coches de una vez: en carrera (y de lejos) se dibuja la geometria del modelo
de garaje (lod0, el que se ve en la seleccion de coche) en vez de la de los modelos de carrera de la DS,
refinada en Blender. Es lo mismo que se hizo a mano con el Focus de prueba, para los 41 coches.

Uso: .venv\\Scripts\\python.exe tools\\models\\upgrade_cars.py [--rom C:\\nfs] [--cars Camaro,Focus] [--level 1]

Que hace con cada coche:
 1. Saca sus modelos de la ROM (tools/extract_models.py) a <modelos>\\_auto\\<coche>.
 2. Empareja cada pieza del modelo de carrera (lod1) con las del de garaje que la sustituyen: mismo material
    ("tex_rgb_mat" = la carroceria pintada), el mismo nombre con un 1 ("cam_det" -> "cam_det1": detalles con
    la textura a mitad de tamano) o los cristales ("glass" -> "lodglass"). Solo si las dos texturas son la
    misma imagen (se comparan reducidas): si no, las coordenadas de textura no valdrian y la pieza se deja.
    Las luces (freno, marcha atras, policia) no se tocan: el modelo de garaje no las tiene.
 3. Ruedas: wheel_lod0 sustituye a wheel_lod1, wheel_lod2 y wheel_lod3.
 4. Blender (tools/models/blender_batch.py): suelda, normales suaves con aristas vivas de mas de 35 grados, y
    la carroceria de lod1 (los coches de cerca) se subdivide una vez. Para lod2 y lod3 (de lejos) la misma
    geometria sin subdividir: asi no se pasa del limite de poligonos por fotograma con muchos coches.
 5. tools/models/make_replacement.py escribe las piezas (.nmesh) en <modelos>\\reemplazos. Antes se guarda
    una copia de la carpeta (reemplazos_antes_<fecha>).
Todas las piezas llevan la bandera de no subdividir en el port (la subdivision al vuelo redondeaba los pilotos).
El modelo de garaje (lod0) tambien se sustituye por su version refinada (seleccion de coche).
Las piezas compartidas por varios coches con sustitutos distintos se dejan como estan.
Despues hay que volver a ejecutar upgrade_lods.py? No: este script ya cubre lod2/lod3; upgrade_lods.py solo
hace falta para las luces (se conservan: no se tocan sus archivos).
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import zipfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nsbmd  # noqa: E402

LIGHTS = {'blight', 'rlight', 'hlight', 'clight', 'plights', 'lambert1'}
PUSH = 0.01  # metros (el coche mide unos 5)
BLENDER = r'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe'


def model_info(z, names, car, lod):
    key = 'graphics/cars/%s/%s.nsbmd' % (car.lower(), lod)
    if key not in names:
        return None
    models, tex0 = nsbmd.load(z.read(names[key]))
    m = models[0]
    shapes = []
    for node, mat, shp in m.draws:
        mt = m.materials[mat] if mat is not None else {'name': None, 'tex': None, 'pltt': None}
        s = m.shapes[shp]
        shapes.append({'shape': s['name'], 'hash': '%016x' % s['dl_hash'], 'mat': mt['name'], 'tex': mt['tex'],
                       'pltt': mt['pltt'], 'tris': len(s['tris']), 'verts': s['verts']})
    return {'shapes': shapes, 'tex0': tex0}


def tex_image(info, s):
    try:
        return info['tex0'].image(s['tex'], s['pltt'])
    except Exception:
        return None


def same_image(a_info, a, b_info, b):
    """La textura de la pieza de garaje y la de carrera son la misma imagen (a otro tamano)"""
    if a['tex'] is None or b['tex'] is None:
        return a['tex'] is None and b['tex'] is None
    ia, ib = tex_image(a_info, a), tex_image(b_info, b)
    if ia is None or ib is None:
        return False
    h, w = ib.shape[:2]
    ra = np.asarray(Image.fromarray(ia, 'RGBA').resize((w, h), Image.BOX), np.float32)
    rb = ib.astype(np.float32)
    mask = (ra[..., 3] > 0) & (rb[..., 3] > 0)
    if mask.sum() < 0.3 * w * h:
        return False
    diff = np.abs(ra[..., :3] - rb[..., :3])[mask].mean()
    if diff < 28:
        return True
    # misma disposicion con el dibujo algo retocado (p. ej. GT500): se compara la forma (luminancia)
    la, lb = ra[..., :3].mean(-1)[mask], rb[..., :3].mean(-1)[mask]
    la, lb = la - la.mean(), lb - lb.mean()
    den = np.sqrt((la * la).sum() * (lb * lb).sum())
    return den > 0 and (la * lb).sum() / den > 0.6


def bbox(info):
    pts = np.array([v['pos'] for s in info['shapes'] if s['mat'] not in LIGHTS and 'shadow' not in (s['mat'] or '') for v in s['verts']])
    return pts.min(0), pts.max(0)


def sources_for(lod0, lod0_info, s1, lod1_info):
    """Piezas del modelo de garaje que sustituyen a la pieza s1 del de carrera"""
    m = s1['mat']
    if m in LIGHTS or m is None:
        return []
    if m == 'lodglass':
        return [s for s in lod0 if s['mat'] == 'glass']
    cands = [s for s in lod0 if s['mat'] == m]
    if not cands:
        cands = [s for s in lod0 if s['mat'] and s['mat'] + '1' == m]
    if not cands:  # p. ej. GTtex256 -> textura GTtex128
        def stem(t):
            return (t or '').lower().replace('256', '').replace('128', '').rstrip('0123456789_')
        cands = [s for s in lod0 if s['mat'] not in LIGHTS and s['mat'] != 'glass' and s['tex'] and stem(s['tex']) == stem(s1['tex'])]
    return [s for s in cands if same_image(lod0_info, s, lod1_info, s1)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', default=r'C:\nfs')
    ap.add_argument('--models', default=os.environ.get('NFS_MODELS', os.path.expanduser(r'~\nfs-nitro-models')))
    ap.add_argument('--cars', default='')
    ap.add_argument('--level', type=int, default=1)
    ap.add_argument('--blender', default=BLENDER)
    args = ap.parse_args()
    z = zipfile.ZipFile(os.path.join(args.rom, 'data', 'vfs.zip'))
    names = {n.lower(): n for n in z.namelist()}
    cars = sorted(set(names['graphics/cars/%s/lod0.nsbmd' % n.split('/')[2]].split('/')[2]
                      for n in names if n.startswith('graphics/cars/') and n.endswith('/lod0.nsbmd') and n.count('/') == 3))
    if args.cars:
        want = {c.lower() for c in args.cars.split(',')}
        cars = [c for c in cars if c.lower() in want]
    work = os.path.join(args.models, '_auto')
    out = os.path.join(args.models, 'reemplazos')
    jobs, empty, owners, report = [], [], {}, []
    for car in cars:
        info = {lod: model_info(z, names, car, lod) for lod in
                ('lod0', 'lod1', 'lod2', 'lod3', 'wheel_lod0', 'wheel_lod1', 'wheel_lod2', 'wheel_lod3')}
        if not info['lod0'] or not info['lod1']:
            continue
        # el modelo de garaje tiene que estar en el mismo sitio que el de carrera
        (a0, a1), (b0, b1) = bbox(info['lod0']), bbox(info['lod1'])
        if np.abs(a0 - b0).max() > 0.08 * np.abs(b1 - b0).max() or np.abs(a1 - b1).max() > 0.08 * np.abs(b1 - b0).max():
            report.append('%s: el modelo de garaje no esta donde el de carrera: se deja' % car)
            continue
        cdir = os.path.join(work, car)
        if not os.path.exists(os.path.join(cdir, 'lod0.obj')):
            subprocess.check_call([sys.executable, os.path.join(os.path.dirname(HERE), 'extract_models.py'),
                                   'Graphics/Cars/' + car, '--rom', args.rom, '--out', cdir], stdout=subprocess.DEVNULL)
        lod0 = info['lod0']['shapes']
        near, far = {}, {}  # forma de lod0 -> piezas del juego (cerca: subdividida; lejos: no)
        smooth_src = None
        body = [s for s in lod0 if s['mat'] == 'tex_rgb_mat'] or sorted(
            [s for s in lod0 if s['mat'] not in LIGHTS and s['mat'] != 'glass' and 'shadow' not in (s['mat'] or '')], key=lambda s: -s['tris'])[:1]
        if body:
            smooth_src = body[0]['shape']
        used, skipped = {}, []
        by_mat = {}
        for s1 in info['lod1']['shapes']:
            src = sources_for(lod0, info['lod0'], s1, info['lod1'])
            if not src:
                if s1['mat'] == 'lodglass' and not any(s['mat'] == 'glass' for s in lod0):
                    empty.append(s1['hash'])  # el modelo de garaje lleva los cristales en la carroceria
                    by_mat['lodglass'] = None
                elif s1['mat'] not in LIGHTS:
                    skipped.append(s1['mat'])
                continue
            key = tuple(s['shape'] for s in src)
            if key in used:  # dos piezas de carrera con el mismo material: la mayor lleva todo, la otra nada
                prev = used[key]
                if s1['tris'] > prev['tris']:
                    empty.append(prev['hash'])
                    for s in src:
                        near[s['shape']].remove('%s__%s' % (prev['shape'], prev['hash']))
                    used[key] = s1
                else:
                    empty.append(s1['hash'])
                    continue
            else:
                used[key] = s1
            for s in src:
                near.setdefault(s['shape'], []).append('%s__%s' % (s1['shape'], s1['hash']))
            by_mat[s1['mat']] = src
        for lod in ('lod2', 'lod3'):  # de lejos: por material, como upgrade_lods.py
            if not info[lod]:
                continue
            for s2 in info[lod]['shapes']:
                if s2['mat'] in by_mat and by_mat[s2['mat']] is None:
                    empty.append(s2['hash'])
                elif s2['mat'] in by_mat:
                    for s in by_mat[s2['mat']]:
                        far.setdefault(s['shape'], []).append('%s__%s' % (s2['shape'], s2['hash']))
        # el propio modelo de garaje (seleccion de coche) tambien: sin esto el port lo subdividia al vuelo y la
        # trasera salia deformada (pilotos doblados) y su pintura no pasaba por el filtro de calcomanias
        for s in lod0:
            if s['shape'] in near:
                near[s['shape']].append('%s__%s' % (s['shape'], s['hash']))
        if [m for m in skipped if m != 'lodglass']:  # una pieza sin pareja quedaria mezclada con el modelo nuevo
            report.append('%s: sin pareja para %s (otra textura): el coche se deja como esta' % (car, ', '.join(skipped)))
            empty = [h for h in empty if h not in {s['hash'] for lod in ('lod1', 'lod2', 'lod3') if info[lod] for s in info[lod]['shapes']}]
            continue
        targets = [t for v in list(near.values()) + list(far.values()) for t in v]
        wheel_targets = []
        if info['wheel_lod0'] and info['wheel_lod1']:
            w0, w1 = info['wheel_lod0']['shapes'], info['wheel_lod1']['shapes']
            if len(w0) == 1 and len(w1) == 1 and same_image(info['wheel_lod0'], w0[0], info['wheel_lod1'], w1[0]):
                for lod in ('wheel_lod1', 'wheel_lod2', 'wheel_lod3'):
                    if info[lod]:
                        wheel_targets += ['%s__%s' % (s['shape'], s['hash']) for s in info[lod]['shapes']]
                if wheel_targets:
                    jobs.append({'car': car, 'src': os.path.join(cdir, 'wheel_lod0.obj'),
                                 'dst': os.path.join(cdir, 'auto_wheel.obj'), 'sharp': 35,
                                 'objects': [{'src': w0[0]['shape'], 'targets': wheel_targets, 'smooth': False}]})
            else:
                skipped.append('ruedas')
        for t in targets + wheel_targets:
            owners.setdefault(t.split('__')[1], set()).add(car)
        # los detalles y cristales del modelo de garaje estan en el mismo plano que la chapa; en carrera el juego los
        # dibuja antes que la carroceria y esta los tapaba: se separan 1 cm hacia fuera
        objs = [{'src': k, 'targets': v, 'smooth': k == smooth_src, 'push': 0 if k == smooth_src else PUSH} for k, v in near.items() if v]
        if objs:
            jobs.append({'car': car, 'src': os.path.join(cdir, 'lod0.obj'), 'dst': os.path.join(cdir, 'auto_near.obj'),
                         'sharp': 35, 'level': args.level, 'objects': objs})
        objs = [{'src': k, 'targets': v, 'smooth': False, 'push': 0 if k == smooth_src else PUSH} for k, v in far.items() if v]
        if objs:
            jobs.append({'car': car, 'src': os.path.join(cdir, 'lod0.obj'), 'dst': os.path.join(cdir, 'auto_far.obj'),
                         'sharp': 35, 'objects': objs})
        report.append('%s: %d piezas de cerca, %d de lejos, %d ruedas%s' % (
            car, sum(len(v) for v in near.values()), sum(len(v) for v in far.values()), len(wheel_targets),
            (' (sin pareja: %s)' % ', '.join(skipped)) if skipped else ''))
    shared = {h for h, c in owners.items() if len(c) > 1}
    if shared:  # piezas compartidas: no se sabe de que coche poner la geometria
        for job in jobs:
            for o in job['objects']:
                o['targets'] = [t for t in o['targets'] if t.split('__')[1] not in shared]
            job['objects'] = [o for o in job['objects'] if o['targets']]
        jobs = [j for j in jobs if j['objects']]
    os.makedirs(work, exist_ok=True)
    jf = os.path.join(work, 'trabajo.json')
    json.dump(jobs, open(jf, 'w', encoding='utf-8'), indent=1)
    print('\n'.join(report))
    print('piezas compartidas entre coches (se dejan): %d' % len(shared))
    print('Blender: %d tareas...' % len(jobs))
    r = subprocess.run([args.blender, '-b', '--factory-startup', '--python', os.path.join(HERE, 'blender_batch.py'), '--', jf],
                       capture_output=True, text=True, errors='replace')
    done = [l for l in r.stdout.splitlines() if l.startswith('LOTE')]
    if len(done) != len(jobs):
        print(r.stdout[-3000:], r.stderr[-3000:])
        sys.exit('Blender no termino todas las tareas (%d de %d)' % (len(done), len(jobs)))
    if os.path.isdir(out):
        backup = out + '_antes_' + datetime.datetime.now().strftime('%Y%m%d_%H%M%S')
        shutil.copytree(out, backup)
        print('copia de la carpeta de reemplazos en', backup)
    os.makedirs(out, exist_ok=True)
    total = 0
    for job in jobs:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'make_replacement.py'), job['dst'], '--info',
                            os.path.dirname(job['dst']), '--out', out, '--runtime-subdiv', 'no'], capture_output=True, text=True, errors='replace')
        if r.returncode:
            print('fallo en', job['dst'], r.stdout[-1000:], r.stderr[-1000:])
            continue
        total += sum(1 for l in r.stdout.splitlines() if ' triangulos ' in l)
    for h in empty:
        if h in shared:
            continue
        with open(os.path.join(out, h + '.nmesh'), 'wb') as f:  # pieza que ya dibuja otra: no se dibuja
            f.write(b'NMS1' + (8).to_bytes(4, 'little') + (0).to_bytes(4, 'little'))
    print('hecho: %d piezas (+%d vacias) en %s' % (total, len(empty), out))


if __name__ == '__main__':
    main()
