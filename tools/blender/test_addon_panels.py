"""Headless test of the add-on's stage 2 panels (tools/blender/rasaedit_addon/panels.py).
blender --background --factory-startup --python test_addon_panels.py -- <RasaEdit export .gltf, schema beside its folder> <out.gltf>
Imports with the add-on, checks the schema was found and the panels' groups were filled, then edits through the panels' operators and
properties (effect change, a float parameter, a texture slot, piece group / swap set / user data, a contact point's connection point
and light template), checks the JSON custom properties follow, makes a new contact point, and exports with the add-on."""
import bpy, sys, os, json, addon_utils

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("rasaedit_addon", default_set=True)
bpy.ops.rasaedit.import_gltf(filepath=src)
scene = bpy.context.scene
print("PANELS schema path:", scene.get("rasaSchemaPath"))

materials = [m for m in bpy.data.materials if "rasaMaterial" in m.keys()]
meshes = [o for o in scene.objects if o.type == 'MESH' and "rasaPiece" in o.data.keys()]
points = [o for o in scene.objects if o.type == 'EMPTY' and "rasaContactPoint" in o.keys()]
print("PANELS loaded: materials %d/%d, pieces %d/%d, contact points %d/%d" % (
    sum(m.rasa.loaded for m in materials), len(materials), sum(o.data.rasa_piece.loaded for o in meshes), len(meshes),
    sum(o.rasa_cp.loaded for o in points), len(points)))

# Material: change the effect (parameters reconciled with the schema), then a float and a texture slot.
mat = materials[0]
obj = next(o for o in meshes if any(s.material == mat for s in o.material_slots))
before = json.loads(mat["rasaMaterial"])
print("PANELS material %s effect %s, %d parameters" % (mat.name, before["effect"], len(before["parameters"])))
with bpy.context.temp_override(material=mat, object=obj):
    bpy.ops.rasaedit.set_effect(effect="TRDiffMapDetMapVertColor.fx")
after = json.loads(mat["rasaMaterial"])
print("PANELS after set_effect: effect %s, %d parameters, keys %s" % (after["effect"], len(after["parameters"]),
                                                                     [p[0] for p in after["parameters"]][:12]))
p = next(p for p in mat.rasa.params if p.key == "MatDiffuse")
p.f[0] = 0.25
tex = next((p for p in mat.rasa.params if p.ptype in (5, 8) and p.key not in ("DiffuseTexture", "NormalMapTexture")), None)
if tex:
    with bpy.context.temp_override(material=mat, object=obj):
        bpy.ops.rasaedit.pick(owner="material", field="param", index=list(mat.rasa.params).index(tex), list_name="textures",
                              item="arch_hum_metal_decals.dds")
final = {e[0]: e for e in json.loads(mat["rasaMaterial"])["parameters"]}
print("PANELS JSON MatDiffuse %s, %s %s" % (final["MatDiffuse"][2], tex.key if tex else "-", final[tex.key][2] if tex else "-"))

# Piece.
piece = obj.data.rasa_piece
piece.group = "panel test group"
with bpy.context.temp_override(object=obj):
    bpy.ops.rasaedit.pick(owner="piece", field="swap", list_name="swapSets", item="3")
    bpy.ops.rasaedit.add_pair(owner="piece", key="LOD", value="1")
d = json.loads(obj.data["rasaPiece"])
print("PANELS piece JSON group %r swapId %s userData %s" % (d["group"], d["swapId"], d["userData"]))

# Contact point: connection point name and a light template.
if points:
    cp = points[0]
    g = cp.rasa_cp
    names = [i for i, v in enumerate(g.values) if v.key.lower() == "cpname"]
    with bpy.context.temp_override(object=cp):
        if names:
            bpy.ops.rasaedit.pick(owner="cp", field="cpname", index=names[0], list_name="connectionPoints", item="VFX2")
        bpy.ops.rasaedit.add_pair(owner="cp", key="lsys", value="%d," % g.cp_id)
        bpy.ops.rasaedit.pick(owner="cp", field="value", index=len(g.values) - 1, list_name="lightTemplates",
                              item="point_bane_rotatinglight_orange.ltmp")
    print("PANELS contact point %s JSON %s" % (cp.name, json.loads(cp["rasaContactPoint"])))

# A new contact point on an empty.
empty = bpy.data.objects.new("new socket", None)
scene.collection.objects.link(empty)
with bpy.context.temp_override(object=empty):
    bpy.ops.rasaedit.make_contact_point()
print("PANELS new contact point JSON %s" % json.loads(empty["rasaContactPoint"]))

bpy.ops.rasaedit.check()
check = json.loads(scene["rasaCheck"])
for k in ("errors", "warnings"):
    for line in check[k][:6]:
        print("PANELS check %s: %s" % (k, line))
os.makedirs(os.path.dirname(out), exist_ok=True)
bpy.ops.rasaedit.export_gltf(filepath=out)
print("PANELS exported", out, os.path.exists(out))
