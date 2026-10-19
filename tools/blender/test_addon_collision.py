"""Headless test of the add-on's collision tools (tools/blender/rasaedit_addon/collision.py).
blender --background --factory-startup --python test_addon_collision.py -- <RasaEdit export .gltf> <out folder>
For each shape (box, hull, sphere): imports, deletes the model's collision meshes, makes collision from the first piece on the
armature's first bone, exports <out>/<shape>/model.gltf; prints the made mesh's kind / vertices / bounds in the bone's frame. Also
checks the merge note for two collision meshes on one bone."""
import bpy, sys, os, json, addon_utils

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

for shape in ("BOX", "HULL", "SPHERE"):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon_utils.enable("rasaedit_addon", default_set=True)
    bpy.ops.rasaedit.import_gltf(filepath=src, game_preview=False)
    from rasaedit_addon import is_collision
    for o in [o for o in bpy.data.objects if o.type == 'MESH' and is_collision(o)]:
        bpy.data.objects.remove(o)
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
    piece = next(o for o in bpy.data.objects if o.type == 'MESH' and "rasaPiece" in o.data.keys())
    for o in bpy.data.objects:
        o.select_set(False)
    piece.select_set(True)
    bone = arm.data.bones[0].name
    bpy.ops.rasaedit.make_collision(shape=shape, bone=bone)
    made = next(o for o in bpy.data.objects if o.type == 'MESH' and is_collision(o))
    frame = (arm.matrix_world @ arm.data.bones[bone].matrix_local).inverted()
    pts = [frame @ (made.matrix_world @ v.co) for v in made.data.vertices]
    lo = [round(min(p[i] for p in pts), 4) for i in range(3)]
    hi = [round(max(p[i] for p in pts), 4) for i in range(3)]
    print("COLLISION %s: %s, %d vertices, %d faces, bone frame bounds %s .. %s, in %s" % (
        shape, json.loads(made["rasaCollision"]), len(made.data.vertices), len(made.data.polygons), lo, hi,
        [c.name for c in made.users_collection]))
    path = os.path.join(out, shape.lower(), "model.gltf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.rasaedit.export_gltf(filepath=path)
    print("COLLISION exported", path, os.path.exists(path))

# Two collision meshes on one bone: the merge note.
piece.select_set(True)
bpy.ops.rasaedit.make_collision(shape="BOX", bone=bone)
bpy.ops.rasaedit.check()
print("COLLISION notes:", [n for n in json.loads(bpy.context.scene["rasaCheck"])["notes"] if "collision" in n])
