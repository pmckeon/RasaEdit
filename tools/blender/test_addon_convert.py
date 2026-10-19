"""Headless test of Convert to walkable surface (tools/blender/rasaedit_addon/collision.py).
blender --background --factory-startup --python test_addon_convert.py -- <RasaEdit export .gltf of a model with a box / sphere volume> <out .gltf>
Imports the export, lists the collision meshes and their kinds, runs the convert operator with nothing selected (every box / sphere
volume in the scene), checks every collision mesh is now a triangulated walkable surface, runs the check (no box / sphere warning
may remain) and exports for RasaEdit's import."""
import bpy, sys, os, json, addon_utils

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("rasaedit_addon", default_set=True)
bpy.ops.rasaedit.import_gltf(filepath=src, game_preview=False)
from rasaedit_addon import is_collision, panels


def kinds():
    found = []
    for o in bpy.data.objects:
        if o.type == 'MESH' and is_collision(o):
            d = panels.rasa_json(o, "rasaCollision") or panels.rasa_json(o.data, "rasaCollision") or {}
            found.append((o.name, d.get("kind", "surface"), max(len(p.vertices) for p in o.data.polygons) if o.data.polygons else 0))
    return found


before = kinds()
print("CONVERT before:", before)
for o in bpy.data.objects:
    o.select_set(False)
bpy.ops.rasaedit.convert_collision()
after = kinds()
print("CONVERT after:", after)
ok = bool(before) and any(k in ("box", "sphere") for _, k, _ in before) and all(k == "surface" and n == 3 for _, k, n in after)
bpy.ops.rasaedit.check()
check = json.loads(bpy.context.scene["rasaCheck"])
volume_warnings = [w for w in check["warnings"] if "collision volume" in w]
print("CONVERT check warnings about volumes:", volume_warnings)
ok = ok and not volume_warnings
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
bpy.ops.rasaedit.export_gltf(filepath=out)
print("CONVERT exported", out, os.path.exists(out))
print("CONVERT RESULT", "PASS" if ok else "FAIL")
