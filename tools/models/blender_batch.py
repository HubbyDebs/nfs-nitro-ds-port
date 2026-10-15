"""Refinado en Blender (sin abrir su ventana) de muchos modelos de una vez, para tools/models/upgrade_cars.py.

Uso: blender.exe -b --factory-startup --python tools\\models\\blender_batch.py -- TRABAJO.json

TRABAJO.json: lista de tareas {"src": entrada.obj, "dst": salida.obj, "sharp": grados, "level": niveles,
"objects": [{"src": "polygon2", "targets": ["polygon4__a545b3c9f799f6b1", ...], "smooth": true, "push": 0.01}, ...]}.
"push": mover la pieza esa distancia hacia fuera (por sus normales).
Cada objeto de entrada (por su forma, la parte antes de "__") se suelda, se le ponen normales suaves salvo en
las aristas vivas (mas de 'sharp' grados y bordes abiertos) y, si "smooth", se subdivide (Catmull-Clark)
manteniendo vivas esas aristas. Se exporta una copia por cada pieza del juego que sustituye. Lo mismo que
blender_refine.py, pero en una sola sesion de Blender para todos los coches.
"""
import json
import math
import sys

import bmesh
import bpy

jobs = json.load(open(sys.argv[sys.argv.index('--') + 1], encoding='utf-8'))
for job in jobs:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.obj_import(filepath=job['src'], forward_axis='NEGATIVE_Z', up_axis='Y')
    sharp = math.radians(job.get('sharp', 35))
    level = job.get('level', 1)
    want = {o['src']: o for o in job['objects']}
    made = []
    for o in list(bpy.context.scene.objects):
        base = o.name.split('__')[0]
        if o.type != 'MESH' or base not in want:
            bpy.data.objects.remove(o, do_unlink=True)
            continue
        spec = want[base]
        me = o.data
        bm = bmesh.new()
        bm.from_mesh(me)
        # las listas de la DS llegan como triangulos sueltos: se sueldan los vertices que coinciden
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0005)
        if spec.get('push'):  # pegatinas (pilotos, logos) en el plano de la chapa: un poco hacia fuera para que no las tape
            bm.normal_update()
            for v in bm.verts:
                v.co += v.normal * spec['push']
        if spec['smooth']:
            crease = bm.edges.layers.float.get('crease_edge') or bm.edges.layers.float.new('crease_edge')
            for e in bm.edges:
                if len(e.link_faces) != 2 or e.calc_face_angle(0) > sharp:
                    e[crease] = 1.0
            # los bordes abiertos (huecos de ventanas, pasos de rueda) no se mueven nada: si se redondeaban, la chapa
            # tapaba las esquinas del cristal, que no se subdivide (picos de color en las ventanas del Corolla)
            vcrease = bm.verts.layers.float.get('crease_vert') or bm.verts.layers.float.new('crease_vert')
            for v in bm.verts:
                if any(len(e.link_faces) != 2 for e in v.link_edges):
                    v[vcrease] = 1.0
        bm.to_mesh(me)
        bm.free()
        if spec['smooth']:
            mod = o.modifiers.new('subdiv', 'SUBSURF')
            mod.levels = mod.render_levels = level
            mod.uv_smooth = 'PRESERVE_BOUNDARIES'
            mod.boundary_smooth = 'PRESERVE_CORNERS'
        bpy.context.view_layer.objects.active = o
        o.select_set(True)
        bpy.ops.object.shade_smooth_by_angle(angle=sharp)
        o.select_set(False)
        for k, t in enumerate(spec['targets']):  # una copia por pieza del juego
            c = o if k == 0 else o.copy()  # la copia comparte la malla y lleva los mismos modificadores
            if k:
                bpy.context.scene.collection.objects.link(c)
            made.append((c, t))
    for c, t in made:
        c.name = t
    bpy.ops.wm.obj_export(filepath=job['dst'], forward_axis='NEGATIVE_Z', up_axis='Y', apply_modifiers=True,
                          export_normals=True, export_uv=True, export_colors=True, export_materials=False,
                          export_triangulated_mesh=True)
    dg = bpy.context.evaluated_depsgraph_get()
    print('LOTE', job['dst'], ' '.join('%s:%d' % (c.name.split('__')[0], len(c.evaluated_get(dg).data.polygons)) for c, t in made))
