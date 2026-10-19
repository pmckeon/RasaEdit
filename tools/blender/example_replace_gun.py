"""Example: replace a gun's mesh with a new one while keeping what ties it into the game (skeleton, contact points, material).
blender --background --factory-startup --python example_replace_gun.py -- <RasaEdit export of the gun .gltf> <out .gltf>
Does what you would do by hand: imports the export with the add-on, builds a stand-in gun (an orange striped block slide and grip,
sized from the original's bounds, the grip at HPP_Right_Hand, the slide ending at the muzzle point HPS_VFX1), gives it the
original's material with new Base Color / Normal Map images and a black glow map, parents it to the root bone, deletes the original
mesh, and exports with the add-on. Also renders the original and the stand-in (<out folder>/before.png, after.png)."""
import bpy, bmesh, sys, os, addon_utils, mathutils

args = sys.argv[sys.argv.index('--') + 1:]
src, out = args[0], args[1]
folder = os.path.dirname(os.path.abspath(out))
os.makedirs(os.path.join(folder, "textures"), exist_ok=True)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("rasaedit_addon", default_set=True)
bpy.ops.rasaedit.import_gltf(filepath=src, game_preview=False)
scene = bpy.context.scene

arm = next(o for o in scene.objects if o.type == 'ARMATURE')
original = next(o for o in scene.objects if o.type == 'MESH' and "rasaPiece" in o.data.keys())
grip = next(o for o in scene.objects if o.type == 'EMPTY' and o.name.lower().startswith("hpp_right_hand"))
muzzle = next(o for o in scene.objects if o.type == 'EMPTY' and o.name.lower().startswith("hps_vfx1"))
pts = [original.matrix_world @ v.co for v in original.data.vertices]
lo = mathutils.Vector([min(p[i] for p in pts) for i in range(3)])
hi = mathutils.Vector([max(p[i] for p in pts) for i in range(3)])
g, m = grip.matrix_world.translation, muzzle.matrix_world.translation
print("GUN original bounds", [round(x, 3) for x in lo], [round(x, 3) for x in hi], "grip", [round(x, 3) for x in g], "muzzle", [round(x, 3) for x in m])

# The gun's own axes, from its contact points (models are not all upright: this pistol lies on its side): length runs from the grip
# to the muzzle, thickness is the original's smallest extent, height the remaining axis; the slide is on the muzzle's side of the grip.
length_axis = max(range(3), key=lambda i: abs(m[i] - g[i]))
thick_axis = min((i for i in range(3) if i != length_axis), key=lambda i: hi[i] - lo[i])
height_axis = 3 - length_axis - thick_axis
front = m[length_axis]
back = hi[length_axis] if m[length_axis] < g[length_axis] else lo[length_axis]
up = 1.0 if m[height_axis] > g[height_axis] else -1.0
thickness = hi[thick_axis] - lo[thick_axis]
mid_t = (lo[thick_axis] + hi[thick_axis]) / 2
grip_end = lo[height_axis] if up > 0 else hi[height_axis]
slide_low, slide_high = m[height_axis] - up * 0.035, m[height_axis] + up * 0.025


def box(bm, a0, a1, t, h0, h1):
    """A box from a0 to a1 along the gun, t thick, h0..h1 in height."""
    corners = []
    for ha in (h0, h1):
        for ta in (mid_t - t / 2, mid_t + t / 2):
            for aa in (a0, a1):
                c = [0.0, 0.0, 0.0]
                c[length_axis], c[thick_axis], c[height_axis] = aa, ta, ha
                corners.append(c)
    verts = [bm.verts.new(c) for c in corners]
    for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        bm.faces.new([verts[i] for i in f])


bm = bmesh.new()
box(bm, front, back, 0.8 * thickness, slide_low, slide_high)                           # slide and barrel, ending at the muzzle point
box(bm, g[length_axis] - 0.03, g[length_axis] + 0.03, 0.7 * thickness, grip_end, slide_low)   # grip, around the hand's point
bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
uv = bm.loops.layers.uv.new("UVMap")
for f in bm.faces:                       # box-projected UVs (3 cm per stripe on the texture's 16 stripes)
    n = f.normal
    drop = max(range(3), key=lambda i: abs(n[i]))
    keep = [i for i in range(3) if i != drop]
    for loop in f.loops:
        loop[uv].uv = (loop.vert.co[keep[0]] * 2.0, loop.vert.co[keep[1]] * 2.0)
mesh = bpy.data.meshes.new("standin gun")
bm.to_mesh(mesh)
bm.free()
# The game's neutral vertex colour (128 grey = 1.0 in its x 2 shaders; without one RasaEdit writes the same, but Blender's Color
# Attribute node reads a missing attribute as black).
colour = mesh.color_attributes.new("Color", 'BYTE_COLOR', 'CORNER')
for c in colour.data:
    c.color = (0.502, 0.502, 0.502, 1.0)           # linear: written to glTF as is (128)
standin = bpy.data.objects.new("standin gun", mesh)
original.users_collection[0].objects.link(standin)

# Renders (before / after), the gun seen from its side.
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
centre = (lo + hi) / 2
side = mathutils.Vector((0, 0, 0)); side[thick_axis] = -1
cam.location = centre + side * 1.1
cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
scene.camera = cam
w = bpy.data.worlds.new("w"); w.use_nodes = True
w.node_tree.nodes["Background"].inputs[0].default_value = (0.7, 0.7, 0.7, 1)
scene.world = w
scene.render.engine = 'BLENDER_EEVEE'
scene.render.resolution_x, scene.render.resolution_y = 480, 320
standin.hide_render = True
scene.render.filepath = os.path.join(folder, "before.png")
bpy.ops.render.render(write_still=True)
standin.hide_render = False

# The original's material, with new images: an orange / black striped colour map, a flat normal map; glow off (a black glow map).
material = original.material_slots[0].material
standin.data.materials.append(material)


def image(name, size, pixel):
    img = bpy.data.images.new(name, size, size, alpha=True)
    img.pixels = [c for y in range(size) for x in range(size) for c in pixel(x, y)]
    img.filepath_raw = os.path.join(folder, "textures", name + ".png")
    img.file_format = 'PNG'
    img.save()
    return img


stripes = image("standin_gun_df", 64, lambda x, y: (1.0, 0.45, 0.05, 1.0) if (x // 4) % 2 == 0 else (0.05, 0.05, 0.05, 1.0))
flat = image("standin_gun_nm", 8, lambda x, y: (0.5, 0.5, 1.0, 1.0))
image("standin_gun_black_gm", 8, lambda x, y: (0.0, 0.0, 0.0, 1.0))
nt = material.node_tree
bsdf = next(n for n in nt.nodes if n.bl_idname == "ShaderNodeBsdfPrincipled")


def image_node_into(socket):
    link = socket.links[0]
    node = link.from_node
    while node.bl_idname != "ShaderNodeTexImage":
        node = next(l.from_node for i in node.inputs for l in i.links)
    return node


image_node_into(bsdf.inputs["Base Color"]).image = stripes
image_node_into(bsdf.inputs["Normal"]).image = flat
glow = next((p for p in material.rasa.params if p.key == "GlowTexture"), None)
if glow:
    glow.s = "standin_gun_black_gm.dds"            # read from textures/standin_gun_black_gm.png by RasaEdit's import

# On the root bone (a static piece), then the original mesh goes.
root_bone = next(b.name for b in arm.data.bones if b.parent is None)
world = standin.matrix_world.copy()
standin.parent, standin.parent_type, standin.parent_bone = arm, 'BONE', root_bone
standin.matrix_world = world
print("GUN stand-in: %d vertices, %d faces, on bone %s, material %s" % (len(mesh.vertices), len(mesh.polygons), root_bone, material.name))

bpy.data.objects.remove(original)
scene.render.filepath = os.path.join(folder, "after.png")
bpy.ops.render.render(write_still=True)
bpy.data.objects.remove(cam)

bpy.ops.rasaedit.check()
import json
check = json.loads(scene["rasaCheck"])
print("GUN check:", check)
bpy.ops.rasaedit.export_gltf(filepath=out)
print("GUN exported", out, os.path.exists(out))
