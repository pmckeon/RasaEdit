"""Edit a RasaEdit glTF export in Blender and export it again (the 'user edits in Blender' test).
blender --background --factory-startup --python blender_edit.py -- <in.gltf> <out.gltf>
Edits: the largest mesh is widened 40 % along its own x axis; a 1 m cube with a new checker texture is added, parented to the
armature's bone 'Bip01 Floater' (if present, else the first bone) at the bone's head; the first animation moves that bone 1 m up
at its first key."""
import bpy, bmesh, sys, os, mathutils

args = sys.argv[sys.argv.index('--') + 1:]
src, dst = args[0], args[1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)

meshes = [o for o in bpy.data.objects if o.type == 'MESH']
big = max(meshes, key=lambda o: len(o.data.vertices))
for v in big.data.vertices:
    v.co.x *= 1.4
print('EDIT widened', big.name, len(big.data.vertices), 'vertices')

arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
bone = arm.data.bones.get('Bip01 Floater') or arm.data.bones[0]

# A checker image saved as PNG next to the output.
img = bpy.data.images.new('rasaedit_test_checker', 64, 64, alpha=False)
px = []
for y in range(64):
    for x in range(64):
        on = ((x // 8) + (y // 8)) % 2
        px += [1.0, 0.85, 0.1, 1.0] if on else [0.1, 0.2, 0.9, 1.0]
img.pixels = px
os.makedirs(os.path.dirname(dst), exist_ok=True)
img.filepath_raw = os.path.join(os.path.dirname(dst), 'rasaedit_test_checker.png')
img.file_format = 'PNG'
img.save()

mat = bpy.data.materials.new('checker')
mat.use_nodes = True
tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
tex.image = img
mat.node_tree.links.new(tex.outputs['Color'], mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'])

bm = bmesh.new()
bm.loops.layers.uv.new('UVMap')
bmesh.ops.create_cube(bm, size=1.0, calc_uvs=True)
me = bpy.data.meshes.new('rasaedit_test_cube')
bm.to_mesh(me)
bm.free()
me.materials.append(mat)
cube = bpy.data.objects.new('rasaedit_test_cube', me)
bpy.context.scene.collection.objects.link(cube)
head_world = arm.matrix_world @ bone.head_local
cube.parent = arm
cube.parent_type = 'BONE'
cube.parent_bone = bone.name
cube.matrix_world = mathutils.Matrix.Translation(head_world + mathutils.Vector((0, 0, 1.5)))
print('EDIT cube under bone', bone.name, 'at', tuple(round(c, 3) for c in cube.matrix_world.translation))

# Animation: move the bone 1 m up (Blender z) at its first location key.
if bpy.data.actions:
    act = bpy.data.actions[0]
    moved = 0
    for fc in act.fcurves if hasattr(act, 'fcurves') else []:
        if fc.data_path == 'pose.bones["%s"].location' % bone.name and fc.keyframe_points:
            fc.keyframe_points[0].co.y += 1.0 if fc.array_index == 1 else 0.0
            moved += 1
    print('EDIT animation', act.name, 'location curves touched', moved)

bpy.ops.export_scene.gltf(filepath=dst, export_format='GLTF_SEPARATE', export_extras=True, export_animations=True)
print('EDIT exported', dst)
