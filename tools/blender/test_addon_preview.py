"""Headless test of the add-on's game-like preview (tools/blender/rasaedit_addon/preview.py).
blender --background --factory-startup --python test_addon_preview.py -- <RasaEdit export .gltf> <out folder>
Imports without the preview and exports (a.gltf), imports with it, lists the preview nodes per material, renders a view (preview.png),
exports through File > Export > glTF (b.gltf: the export hook must take the preview out) and checks the materials of a and b are the
same and the preview is back afterwards; then turns the preview off and checks the importer's links are restored."""
import bpy, sys, os, json, addon_utils, math, mathutils

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
os.makedirs(out, exist_ok=True)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def fresh():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    addon_utils.enable("rasaedit_addon", default_set=True)


def links_of(m):
    return sorted("%s.%s>%s.%s" % (l.from_node.name, l.from_socket.identifier, l.to_node.name, l.to_socket.identifier) for l in m.node_tree.links)


def materials_json(path):
    with open(path, encoding="utf-8") as f:
        g = json.load(f)
    return json.dumps(g.get("materials", []), sort_keys=True)


fresh()
bpy.ops.rasaedit.import_gltf(filepath=src, game_preview=False)
plain = {m.name: links_of(m) for m in bpy.data.materials if m.node_tree}
bpy.ops.export_scene.gltf(filepath=os.path.join(out, "a.gltf"), export_format='GLTF_SEPARATE', export_extras=True)

fresh()
bpy.ops.rasaedit.import_gltf(filepath=src, game_preview=True)
scene = bpy.context.scene
for m in bpy.data.materials:
    if m.node_tree:
        nodes = [n.label for n in m.node_tree.nodes if n.name.startswith("RasaEdit preview")]
        print("PREVIEW %s: %d nodes %s" % (m.name, len(nodes), nodes[:10]))

# A view of the model.
objs = [o for o in scene.objects if o.type == 'MESH' and not o.name.lower().startswith("collision")]
lo = mathutils.Vector((1e9, 1e9, 1e9)); hi = -lo
for o in objs:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        lo = mathutils.Vector(map(min, lo, w)); hi = mathutils.Vector(map(max, hi, w))
centre, size = (lo + hi) / 2, max((hi - lo).length, 0.1)
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
cam.location = centre + mathutils.Vector((size * 0.9, -size * 1.1, size * 0.5))
cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
scene.camera = cam
world = bpy.data.worlds.new("w"); world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (0.6, 0.6, 0.6, 1)
scene.world = world
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x, scene.render.resolution_y = 480, 480
for name, on in (("preview_on.png", True), ("preview_off.png", False)):
    scene.rasa_preview = on
    scene.render.filepath = os.path.join(out, name)
    bpy.ops.render.render(write_still=True)
scene.rasa_preview = True

bpy.ops.export_scene.gltf(filepath=os.path.join(out, "b.gltf"), export_format='GLTF_SEPARATE', export_extras=True)
print("PREVIEW exported materials equal (plain vs with preview):", materials_json(os.path.join(out, "a.gltf")) == materials_json(os.path.join(out, "b.gltf")))
print("PREVIEW back after the export:", sum(1 for m in bpy.data.materials if "rasaPreview" in m.keys()), "materials")
scene.rasa_preview = False
same = all(links_of(m) == plain.get(m.name) for m in bpy.data.materials if m.node_tree and m.name in plain)
print("PREVIEW off restores the importer's links:", same)
