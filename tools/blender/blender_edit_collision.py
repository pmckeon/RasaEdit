"""Scale every collision mesh (objects named "collision ...", in the hidden Collision collection) of a RasaEdit export by 2 about its
own origin (the bone), then export again. blender --background --factory-startup --python blender_edit_collision.py -- <in.gltf> <out.gltf>"""
import bpy, sys

args = sys.argv[sys.argv.index('--') + 1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=args[0])
edited = 0
for o in bpy.data.objects:
    if o.type == 'MESH' and o.name.lower().startswith('collision'):
        for v in o.data.vertices:
            v.co *= 2.0
        edited += 1
print('EDIT collision meshes scaled', edited)
bpy.ops.export_scene.gltf(filepath=args[1], export_format='GLTF_SEPARATE', export_extras=True, export_animations=True)
