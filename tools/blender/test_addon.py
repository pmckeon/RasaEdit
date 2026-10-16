"""Headless test of the RasaEdit add-on (tools/blender/rasaedit_addon).
blender --background --factory-startup --python test_addon.py -- <RasaEdit export .gltf> <out folder>
Imports the export at 24 fps with the add-on, reports the frame rate, the hidden collections and the rest pose record, runs the checks,
exports (trip 1), imports that again in a fresh scene (trip 2: the groups come from the add-on, not from glTF scenes), exports again,
then moves a bone and checks that the shared-skeleton warning appears."""
import bpy, sys, os, json, addon_utils

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("rasaedit_addon", default_set=True)
print("ADDON enabled:", "rasaedit_addon" in bpy.context.preferences.addons.keys())


def report(stage):
    scene = bpy.context.scene

    def walk(layer, depth=0):
        meshes = [o for o in layer.collection.objects if o.type == 'MESH']
        print("ADDON %s collection %s%s %s, %d meshes" % (stage, "  " * depth, layer.name, "EXCLUDED" if layer.exclude else "shown", len(meshes)))
        for child in layer.children:
            walk(child, depth + 1)
    walk(bpy.context.view_layer.layer_collection)
    print("ADDON %s fps %s, armatures with rasaRestPose %d" % (stage, scene.render.fps,
          sum(1 for o in bpy.data.objects if o.type == 'ARMATURE' and "rasaRestPose" in o.keys())))


bpy.context.scene.render.fps = 24
bpy.ops.rasaedit.import_gltf(filepath=src)
report("trip1")
bpy.ops.rasaedit.check()
check = json.loads(bpy.context.scene["rasaCheck"])
print("ADDON check errors %d warnings %d notes %d" % (len(check["errors"]), len(check["warnings"]), len(check["notes"])))
for k in ("errors", "warnings", "notes"):
    for line in check[k][:4]:
        print("ADDON   %s: %s" % (k, line))
os.makedirs(out, exist_ok=True)
trip1 = os.path.join(out, "trip1", "model.gltf")
bpy.ops.rasaedit.export_gltf(filepath=trip1)
print("ADDON exported", trip1, os.path.exists(trip1))

bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("rasaedit_addon", default_set=True)
bpy.context.scene.render.fps = 24
bpy.ops.rasaedit.import_gltf(filepath=trip1)
report("trip2")
trip2 = os.path.join(out, "trip2", "model.gltf")
bpy.ops.rasaedit.export_gltf(filepath=trip2)
print("ADDON exported", trip2, os.path.exists(trip2))

# A moved bone triggers the shared-skeleton warning.
arm = [o for o in bpy.data.objects if o.type == 'ARMATURE'][0]
bpy.context.view_layer.objects.active = arm
bpy.ops.object.mode_set(mode='EDIT')
bone = arm.data.edit_bones[len(arm.data.edit_bones) // 2]
bone.head.x += 0.1
bone.tail.x += 0.1
bpy.ops.object.mode_set(mode='OBJECT')
bpy.ops.rasaedit.check()
check = json.loads(bpy.context.scene["rasaCheck"])
print("ADDON after moving %s: %s" % (bone.name, [w for w in check["warnings"] if "rest pose" in w][:1]))
