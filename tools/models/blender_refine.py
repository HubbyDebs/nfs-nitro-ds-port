"""Hace en Blender (sin abrir su ventana) una version refinada de un modelo extraido y la deja lista para
tools/models/make_replacement.py.

Uso:
  blender.exe -b --factory-startup --python tools\\models\\blender_refine.py -- ENTRADA.obj SALIDA.obj
        --map polygon2=polygon4__a545b3c9f799f6b1,polygon1=polygon3__0bdb89b1b607e137
        [--smooth polygon2] [--level 1] [--sharp 35]

--map     que objeto de ENTRADA sustituye a que pieza del juego (forma de entrada = nombre__huella de la
          pieza del juego). Los objetos que no salen aqui se borran. Asi se puede, por ejemplo, poner en
          carrera la geometria del modelo de garaje (lod0) en lugar de la del modelo de carrera (lod1).
--smooth  objetos que se subdividen y redondean (Catmull-Clark). Los demas solo se sueldan.
--level   niveles de subdivision (1 = cada cara en 4). Ojo: el juego dibuja por software; 1 basta.
--sharp   angulo (grados) a partir del cual una arista se considera viva: no se redondea y la luz la
          marca. Tambien se mantienen vivos los bordes abiertos.
"""
import math
import sys

import bmesh
import bpy

args = sys.argv[sys.argv.index('--') + 1:]
src, dst = args[0], args[1]
opts = {'--map': '', '--smooth': '', '--level': '1', '--sharp': '35'}
i = 2
while i < len(args):
    opts[args[i]] = args[i + 1]
    i += 2
mapping = dict(kv.split('=') for kv in opts['--map'].split(',') if kv)
smooth = set(x for x in opts['--smooth'].split(',') if x)
level, sharp = int(opts['--level']), math.radians(float(opts['--sharp']))

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=src, forward_axis='NEGATIVE_Z', up_axis='Y')

for o in list(bpy.context.scene.objects):
    base = o.name.split('__')[0]
    if o.type != 'MESH' or base not in mapping:
        bpy.data.objects.remove(o, do_unlink=True)
        continue
    me = o.data
    bm = bmesh.new()
    bm.from_mesh(me)
    # las listas de la DS llegan como triangulos sueltos: se sueldan los vertices que coinciden
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=0.0005)
    if base in smooth:
        crease = bm.edges.layers.float.get('crease_edge') or bm.edges.layers.float.new('crease_edge')
        for e in bm.edges:
            if len(e.link_faces) != 2 or e.calc_face_angle(0) > sharp:
                e[crease] = 1.0
                e.smooth = False
    bm.to_mesh(me)
    bm.free()
    for p in me.polygons:
        p.use_smooth = True
    for e in me.edges:  # aristas vivas: la luz las marca
        pass
    o.name = mapping[base]
    if base in smooth:
        mod = o.modifiers.new('subdiv', 'SUBSURF')
        mod.levels = mod.render_levels = level
        mod.uv_smooth = 'PRESERVE_BOUNDARIES'
    # normales suaves salvo en las aristas vivas
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    bpy.ops.object.shade_smooth_by_angle(angle=sharp)
    o.select_set(False)

bpy.ops.wm.obj_export(filepath=dst, forward_axis='NEGATIVE_Z', up_axis='Y', apply_modifiers=True,
                      export_normals=True, export_uv=True, export_colors=True, export_materials=False,
                      export_triangulated_mesh=True)
for o in bpy.context.scene.objects:
    if o.type == 'MESH':
        dg = o.evaluated_get(bpy.context.evaluated_depsgraph_get())
        print('REFINADO', o.name, 'caras', len(dg.data.polygons))
