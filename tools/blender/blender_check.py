"""Blender headless check of a RasaEdit glTF export.
blender --background --factory-startup --python blender_check.py -- <in.gltf> <render.png> [<reexport.gltf>]"""
import bpy, sys, math, mathutils

args = sys.argv[sys.argv.index('--') + 1:]
src, render = args[0], args[1]
reexport = args[2] if len(args) > 2 else None

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)

meshes = [o for o in bpy.data.objects if o.type == 'MESH']
arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
empties = [o for o in bpy.data.objects if o.type == 'EMPTY']
print('RESULT objects', len(bpy.data.objects), 'meshes', len(meshes), 'armatures', len(arms), 'empties', len(empties))
print('RESULT vertices', sum(len(o.data.vertices) for o in meshes), 'triangles', sum(sum(len(p.vertices) - 2 for p in o.data.polygons) for o in meshes))
for a in arms:
    print('RESULT armature', a.name, 'bones', len(a.data.bones))
print('RESULT actions', [(act.name, round(act.frame_range[1] - act.frame_range[0], 1)) for act in bpy.data.actions])
print('RESULT materials', len(bpy.data.materials), 'with rasaMaterial', sum(1 for m in bpy.data.materials if 'rasaMaterial' in m.keys()))
print('RESULT meshes with rasaPiece', sum(1 for m in bpy.data.meshes if 'rasaPiece' in m.keys()))
print('RESULT images', len(bpy.data.images), [i.name for i in bpy.data.images][:6])
def walk(lc, depth=0):
    objs = lc.collection.objects
    meshes_in = [o for o in objs if o.type == 'MESH']
    print('RESULT collection', '  ' * depth + lc.name, 'excluded' if lc.exclude else 'shown', len(objs), 'objects', len(meshes_in), 'meshes',
          [(o.name, [m.object.name for m in o.modifiers if m.type == 'ARMATURE']) for o in meshes_in][:4])
    for c in lc.children: walk(c, depth + 1)
walk(bpy.context.view_layer.layer_collection)
for o in empties[:3]:
    print('RESULT empty', o.name, list(o.keys()))

# Render: frame everything with a camera looking from the game's south-west (glTF -z is the game's north).
scene = bpy.context.scene
pts = []
for o in meshes:
    for c in o.bound_box:
        pts.append(o.matrix_world @ mathutils.Vector(c))
if pts:
    lo = mathutils.Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = mathutils.Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    centre = (lo + hi) / 2
    size = (hi - lo).length
    cam = bpy.data.objects.new('cam', bpy.data.cameras.new('cam'))
    scene.collection.objects.link(cam)
    cam.location = centre + mathutils.Vector((size * 0.6, -size * 1.1, size * 0.4))
    direction = centre - cam.location
    cam.rotation_euler = direction.to_track_quat('-Z', 'Y').to_euler()
    scene.camera = cam
    sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
    sun.rotation_euler = (math.radians(50), 0, math.radians(30))
    scene.collection.objects.link(sun)
    scene.render.engine = 'BLENDER_EEVEE'
    world = bpy.data.worlds.new('w'); world.color = (0.6, 0.6, 0.6); scene.world = world
    sun.data.energy = 4
    scene.render.resolution_x = 640
    scene.render.resolution_y = 640
    scene.render.filepath = render
    bpy.ops.render.render(write_still=True)
    print('RESULT rendered', render)

if reexport:
    bpy.ops.export_scene.gltf(filepath=reexport, export_format='GLTF_SEPARATE', export_extras=True, export_animations=True)
    print('RESULT reexported', reexport)
