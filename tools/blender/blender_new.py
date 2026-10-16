"""A brand-new model made in Blender (no RasaEdit data): a 3 m column of 12 rings skinned to two bones, a red / white
texture, and a 2 s action bending the upper bone 60 degrees. blender --background --factory-startup --python blender_new.py -- <out.gltf>"""
import bpy, bmesh, sys, os, math

out = sys.argv[sys.argv.index('--') + 1:][0]
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 30

# Armature: 'base' from 0 to 1.5 m, 'top' from 1.5 to 3 m (Blender z up).
arm_data = bpy.data.armatures.new('rig')
arm = bpy.data.objects.new('rasaedit_new_rig', arm_data)
scene.collection.objects.link(arm)
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='EDIT')
base = arm_data.edit_bones.new('base')
base.head, base.tail = (0, 0, 0), (0, 0, 1.5)
top = arm_data.edit_bones.new('top')
top.head, top.tail = (0, 0, 1.5), (0, 0, 3)
top.parent = base
bpy.ops.object.mode_set(mode='OBJECT')

# Column mesh: 12 rings of 16 vertices, UVs around and up.
me = bpy.data.meshes.new('column')
bm = bmesh.new()
uvl = bm.loops.layers.uv.new('UVMap')
rings = []
for r in range(13):
    z = r * 0.25
    ring = [bm.verts.new((0.3 * math.cos(2 * math.pi * k / 16), 0.3 * math.sin(2 * math.pi * k / 16), z)) for k in range(16)]
    rings.append(ring)
for r in range(12):
    for k in range(16):
        f = bm.faces.new((rings[r][k], rings[r][(k + 1) % 16], rings[r + 1][(k + 1) % 16], rings[r + 1][k]))
        for loop, (du, dv) in zip(f.loops, ((0, 0), (1, 0), (1, 1), (0, 1))):
            loop[uvl].uv = ((k + du) / 16.0, (r + dv) / 12.0)
bm.normal_update()
bm.to_mesh(me)
bm.free()
col = bpy.data.objects.new('column', me)
scene.collection.objects.link(col)
gb = col.vertex_groups.new(name='base')
gt = col.vertex_groups.new(name='top')
for v in me.vertices:
    t = min(1.0, max(0.0, (v.co.z - 1.0) / 1.0))
    if t < 1: gb.add([v.index], 1 - t, 'REPLACE')
    if t > 0: gt.add([v.index], t, 'REPLACE')
mod = col.modifiers.new('rig', 'ARMATURE')
mod.object = arm
col.parent = arm

img = bpy.data.images.new('rasaedit_new_stripes', 64, 64, alpha=False)
px = []
for y in range(64):
    for x in range(64):
        px += [0.9, 0.1, 0.1, 1.0] if (y // 8) % 2 else [0.95, 0.95, 0.95, 1.0]
img.pixels = px
img.filepath_raw = os.path.join(os.path.dirname(out), 'rasaedit_new_stripes.png')
img.file_format = 'PNG'
img.save()
mat = bpy.data.materials.new('stripes')
mat.use_nodes = True
tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
tex.image = img
mat.node_tree.links.new(tex.outputs['Color'], mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'])
me.materials.append(mat)

# Action: the top bone bends 60 degrees about x and back.
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='POSE')
pb = arm.pose.bones['top']
pb.rotation_mode = 'XYZ'
for frame, angle in ((1, 0), (31, 60), (61, 0)):
    pb.rotation_euler = (math.radians(angle), 0, 0)
    pb.keyframe_insert('rotation_euler', frame=frame)
bpy.ops.object.mode_set(mode='OBJECT')
arm.animation_data.action.name = 'rasaedit_new_bend'
scene.frame_start, scene.frame_end = 1, 61

bpy.ops.export_scene.gltf(filepath=out, export_format='GLTF_SEPARATE', export_extras=True, export_animations=True)
print('NEW exported', out)
