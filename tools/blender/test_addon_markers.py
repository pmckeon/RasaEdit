"""Headless test of the add-on's animation markers (tools/blender/rasaedit_addon/markers.py).
blender --background --factory-startup --python test_addon_markers.py -- <RasaEdit export .gltf with animations> <out folder>
Imports, prints each action's pose markers, exports unchanged (<out>/same/model.gltf), then moves the first marker of the first action
with markers 3 frames later, adds a Footstep1 marker at frame 5 and exports again (<out>/edited/model.gltf); prints the markers the
root's rasaModel then carries."""
import bpy, sys, os, json, addon_utils

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("rasaedit_addon", default_set=True)
bpy.ops.rasaedit.import_gltf(filepath=src, game_preview=False)
for a in bpy.data.actions:
    print("MARKERS action %s: %s" % (a.name, [(m.name, m.frame) for m in a.pose_markers]))
for name in ("same", "edited"):
    if name == "edited":
        action = next(a for a in bpy.data.actions if len(a.pose_markers))
        action.pose_markers[0].frame += 3
        action.pose_markers.new("Footstep1").frame = 5
    path = os.path.join(out, name, "model.gltf")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bpy.ops.rasaedit.export_gltf(filepath=path)
    root = next(o for o in bpy.data.objects if "rasaModel" in o.keys())
    for e in json.loads(root["rasaModel"]).get("animations", []):
        print("MARKERS %s %s: %s" % (name, e.get("name"), e.get("markers")))
