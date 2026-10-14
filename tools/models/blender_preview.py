"""Imagen de comprobacion de un OBJ extraido, hecha con Blender sin abrir su ventana.

Uso: blender.exe -b --factory-startup --python tools\\models\\blender_preview.py -- MODELO.obj SALIDA.png [rueda.obj]
Coloca una camara de tres cuartos, luz de estudio y dibuja con las texturas (motor Workbench).
"""
import math
import sys

import bpy
from mathutils import Vector

args = sys.argv[sys.argv.index('--') + 1:]
obj_path, out_path = args[0], args[1]
wheel = args[2] if len(args) > 2 and args[2].endswith(".obj") else None

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=obj_path, forward_axis='NEGATIVE_Z', up_axis='Y')
objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
if wheel:  # una rueda de muestra al lado, para ver su textura
    bpy.ops.wm.obj_import(filepath=wheel, forward_axis='NEGATIVE_Z', up_axis='Y')

# encuadre: caja que envuelve todo
pts = [o.matrix_world @ Vector(c) for o in bpy.context.scene.objects if o.type == 'MESH' for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
center, size = (lo + hi) / 2, (hi - lo).length

cam_data = bpy.data.cameras.new('cam')
cam = bpy.data.objects.new('cam', cam_data)
bpy.context.scene.collection.objects.link(cam)
d = size * 1.25
ang = math.radians(float(args[3])) if len(args) > 3 else math.radians(35)
cam.location = center + Vector((d * math.cos(ang), -d * math.sin(ang), d * 0.45))
direction = center - cam.location
cam.rotation_euler = direction.to_track_quat('-Z', 'Y').to_euler()
bpy.context.scene.camera = cam

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'TEXTURE' if any(o.active_material for o in objs) else 'SINGLE'
scene.display.shading.show_backface_culling = False
scene.render.resolution_x, scene.render.resolution_y = 1000, 700
scene.render.film_transparent = False
scene.world = bpy.data.worlds.new('w')
scene.world.color = (0.55, 0.6, 0.65)
scene.render.filepath = out_path
bpy.ops.render.render(write_still=True)
tris = sum(len(o.data.polygons) for o in objs)
print('PREVIEW OK', out_path, 'objetos', len(objs), 'caras', tris)
