"""Stage 3: a game-like preview of RasaEdit materials in Blender's material nodes.

What the game's shaders do (CLAUDE.md "The game's renderer", "Material maps"), approximated with Blender nodes on top of the glTF
importer's setup (Image -> Mix MULTIPLY Color Attribute -> Principled Base Color):
- vertex colour x 2: VertColor and Pal albedo = texture x vertex colour x 2 (128 = 1.0); glTF has x 1, so models look half as bright
- Hue (Pal*Hue / TRHair / ...Hue effects): texture + (Hue - 0.5) x texture alpha
- detail map (TR*DetMap*): + detail - 0.5, on UV set DetailTexCoord through TexCoordTransform1
- decal map: lerp to the decal by its alpha - TR on DecalTexCoord through TexCoordTransform2 before the vertex colour, Pal on its
  DecalTexCoord set after it
- additive effects (TRAdditive*, Phase Additive): drawn as light added to what is behind (Transparent + Emission x alpha)
Not reproduced: lighting, normal / gloss / environment maps, the aniso facing ramp, glow maps, animated texture transforms.

The preview's nodes are named "RasaEdit preview ..." and the importer's links they replace are recorded on the material
(rasaPreview), so taking the preview out restores the glTF setup exactly. Every glTF export takes it out first (the glTF add-on's
pre_export_hook) and puts it back after, so what is exported is what glTF expects.
"""

import json
import os

import bpy

from . import panels

PREFIX = "RasaEdit preview"
RECORD = "rasaPreview"


def _params(material):
    d = panels.rasa_json(material, "rasaMaterial")
    if not d:
        return "", {}
    return d.get("effect", ""), {p[0]: p[2] for p in d.get("parameters", []) if len(p) >= 3}


def _node(nt, kind, label, x, y):
    n = nt.nodes.new(kind)
    n.name = "%s %s" % (PREFIX, label)
    n.label = label
    n.location = (x, y)
    return n


def _record_and_unlink(nt, link, removed):
    removed.append([link.from_node.name, link.from_socket.identifier, link.to_node.name, link.to_socket.identifier])
    nt.links.remove(link)


def _socket(sockets, identifier):
    for s in sockets:
        if s.identifier == identifier:
            return s
    return None


def _uv_vector(nt, mesh_uv_names, uv_set, transform, x, y):
    """A UV Map node for the game's UV set `uv_set`, through a TexCoordTransform (u' = e0 u + e4 v + e8, v' = e1 u + e5 v + e9 on
    the game's coordinates; Blender's v is 1 - the game's)."""
    uv = _node(nt, "ShaderNodeUVMap", "UV set %d" % uv_set, x - 900, y)
    if mesh_uv_names:
        uv.uv_map = mesh_uv_names[min(uv_set, len(mesh_uv_names) - 1)]
    e = list(transform) if transform and len(transform) == 16 else None
    if e is None or e == [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]:
        return uv.outputs["UV"]
    sep = _node(nt, "ShaderNodeSeparateXYZ", "split uv", x - 750, y)
    nt.links.new(uv.outputs["UV"], sep.inputs[0])

    def math(op, a, b, label, dx, dy, c=None):
        n = _node(nt, "ShaderNodeMath", label, x - 600 + dx, y + dy)
        n.operation = op
        for k, v in enumerate((a, b, c)):
            if v is None:
                continue
            if isinstance(v, (int, float)):
                n.inputs[k].default_value = v
            else:
                nt.links.new(v, n.inputs[k])
        return n.outputs[0]

    v_game = math('SUBTRACT', 1.0, sep.outputs["Y"], "v game", 0, -40)
    u2 = math('MULTIPLY_ADD', sep.outputs["X"], e[0], "u'", 150, 40, math('MULTIPLY_ADD', v_game, e[4], "e4 v + e8", 0, 60, e[8]))
    v2 = math('MULTIPLY_ADD', sep.outputs["X"], e[1], "v'", 150, -60, math('MULTIPLY_ADD', v_game, e[5], "e5 v + e9", 0, -120, e[9]))
    vb = math('SUBTRACT', 1.0, v2, "v blender", 300, -60)
    comb = _node(nt, "ShaderNodeCombineXYZ", "uv'", x - 150, y)
    nt.links.new(u2, comb.inputs[0])
    nt.links.new(vb, comb.inputs[1])
    return comb.outputs[0]


def _image(name, folder):
    """The slot's texture as the PNG RasaEdit wrote beside the model (textures/<name>.png), or None - its own copy read as raw values
    (Non-Color): the game sums and blends texture values as stored, with no sRGB conversion."""
    if not name or not folder:
        return None
    stem = os.path.splitext(os.path.basename(name))[0]
    label = "%s (raw)" % stem
    if label in bpy.data.images:
        return bpy.data.images[label]
    for candidate in (os.path.join(folder, "textures", stem + ".png"), os.path.join(folder, stem + ".png")):
        if os.path.isfile(candidate):
            image = bpy.data.images.load(candidate, check_existing=False)
            image.name = label
            image.colorspace_settings.name = 'Non-Color'
            return image
    return None


def has_preview(material):
    return material.node_tree is not None and RECORD in material.keys()


def remove(material):
    """Takes the preview out, restoring the importer's links."""
    if material.node_tree is None or RECORD not in material.keys():
        return False
    nt = material.node_tree
    try:
        removed = json.loads(material[RECORD])
    except ValueError:
        removed = []
    for n in [n for n in nt.nodes if n.name.startswith(PREFIX)]:
        nt.nodes.remove(n)
    for from_node, from_id, to_node, to_id in removed:
        a, b = nt.nodes.get(from_node), nt.nodes.get(to_node)
        if a and b:
            fs, ts = _socket(a.outputs, from_id), _socket(b.inputs, to_id)
            if fs and ts:
                nt.links.new(fs, ts)
    del material[RECORD]
    return True


def build(material, mesh_uv_names, folder):
    """Adds the preview to a material with RasaEdit data (rasaMaterial); False when there is nothing to do."""
    if material.node_tree is None or has_preview(material):
        return False
    effect, params = _params(material)
    if not effect:
        return False
    nt = material.node_tree
    bsdf = next((n for n in nt.nodes if n.bl_idname == "ShaderNodeBsdfPrincipled"), None)
    output = next((n for n in nt.nodes if n.bl_idname == "ShaderNodeOutputMaterial" and n.is_active_output), None)
    if bsdf is None or output is None:
        return False
    base_in = bsdf.inputs["Base Color"]
    if not base_in.is_linked:
        return False
    removed = []
    lower = effect.lower()
    pal = lower.startswith("pal")
    additive = lower.startswith("tradditive") or str(params.get("Phase", "")).lower() == "additive"
    x, y = bsdf.location.x - 300, bsdf.location.y + 300

    # The importer's chain: Image -> (Mix MULTIPLY with the Color Attribute) -> Base Color.
    base_link = base_in.links[0]
    src = base_link.from_socket
    mix = src.node if src.node.bl_idname == "ShaderNodeMix" and getattr(src.node, "blend_type", "") == "MULTIPLY" else None
    a_in = _socket(mix.inputs, "A_Color") if mix else None
    image_link = a_in.links[0] if a_in is not None and a_in.is_linked else (base_link if src.node.bl_idname == "ShaderNodeTexImage" else None)
    image_node = image_link.from_node if image_link and image_link.from_node.bl_idname == "ShaderNodeTexImage" else None

    # Albedo before the vertex colour: Hue, then (TR) detail and decal.
    if image_link is not None and image_node is not None:
        target = image_link.to_socket
        changed = False
        # The sums below act on stored texture values: Blender has the base texture in linear light, so it goes back to (about)
        # stored values first and to linear after (sRGB approximated by gamma 2.2).
        to_raw = _node(nt, "ShaderNodeGamma", "to stored values", x - 600, y + 300)
        to_raw.inputs["Gamma"].default_value = 1.0 / 2.2
        nt.links.new(image_link.from_socket, to_raw.inputs["Color"])
        colour = to_raw.outputs[0]
        hue = params.get("Hue")
        if "hue" in lower and isinstance(hue, list) and len(hue) >= 3:
            offset = _node(nt, "ShaderNodeRGB", "Hue - 0.5", x - 700, y + 200)
            offset.outputs[0].default_value = (hue[0] - 0.5, hue[1] - 0.5, hue[2] - 0.5, 1.0)
            n = _node(nt, "ShaderNodeVectorMath", "+ (Hue - 0.5) x alpha", x - 450, y + 150)
            n.operation = 'MULTIPLY_ADD'
            nt.links.new(offset.outputs[0], n.inputs[0])
            nt.links.new(image_node.outputs["Alpha"], n.inputs[1])
            nt.links.new(colour, n.inputs[2])
            colour, changed = n.outputs[0], True
        if not pal:
            detail = _image(params.get("DetailTexture"), folder) if "detmap" in lower or "detgloss" in lower else None
            if detail is not None:
                t = _node(nt, "ShaderNodeTexImage", "detail map", x - 700, y - 150)
                t.image = detail
                nt.links.new(_uv_vector(nt, mesh_uv_names, int(params.get("DetailTexCoord", 1)), params.get("TexCoordTransform1"), x - 700, y - 150),
                             t.inputs["Vector"])
                n = _node(nt, "ShaderNodeVectorMath", "+ detail", x - 450, y - 100)
                n.operation = 'ADD'
                nt.links.new(colour, n.inputs[0])
                nt.links.new(t.outputs["Color"], n.inputs[1])
                h = _node(nt, "ShaderNodeVectorMath", "- 0.5", x - 300, y - 100)
                h.operation = 'SUBTRACT'
                nt.links.new(n.outputs[0], h.inputs[0])
                h.inputs[1].default_value = (0.5, 0.5, 0.5)
                colour, changed = h.outputs[0], True
            decal = _image(params.get("DecalTexture"), folder) if "decmap" in lower else None
            if decal is not None:
                t = _node(nt, "ShaderNodeTexImage", "decal map", x - 700, y - 450)
                t.image = decal
                nt.links.new(_uv_vector(nt, mesh_uv_names, int(params.get("DecalTexCoord", 2)), params.get("TexCoordTransform2"), x - 700, y - 450),
                             t.inputs["Vector"])
                m = _node(nt, "ShaderNodeMix", "lerp to decal", x - 200, y - 300)
                m.data_type = 'RGBA'
                nt.links.new(t.outputs["Alpha"], m.inputs["Factor"])
                nt.links.new(colour, _socket(m.inputs, "A_Color"))
                nt.links.new(t.outputs["Color"], _socket(m.inputs, "B_Color"))
                colour, changed = _socket(m.outputs, "Result_Color"), True
        if changed:
            to_linear = _node(nt, "ShaderNodeGamma", "to linear", x - 100, y + 300)
            to_linear.inputs["Gamma"].default_value = 2.2
            nt.links.new(colour, to_linear.inputs["Color"])
            _record_and_unlink(nt, image_link, removed)
            nt.links.new(to_linear.outputs[0], target)
        else:
            nt.nodes.remove(to_raw)

    # x 2 (vertex colour 128 = 1.0), then (Pal) the decal on top.
    base_link = base_in.links[0]
    final = base_link.from_socket
    _record_and_unlink(nt, base_link, removed)
    double = _node(nt, "ShaderNodeVectorMath", "x 2", x - 50, y)
    double.operation = 'SCALE'
    double.inputs["Scale"].default_value = 2.0
    nt.links.new(final, double.inputs[0])
    final = double.outputs[0]
    if pal and "dec" in lower:
        decal = _image(params.get("DecalTexture"), folder)
        if decal is not None:
            t = _node(nt, "ShaderNodeTexImage", "decal map", x - 350, y - 250)
            t.image = decal
            uv = _node(nt, "ShaderNodeUVMap", "UV set %d" % int(params.get("DecalTexCoord", 0)), x - 550, y - 250)
            if mesh_uv_names:
                uv.uv_map = mesh_uv_names[min(int(params.get("DecalTexCoord", 0)), len(mesh_uv_names) - 1)]
            nt.links.new(uv.outputs["UV"], t.inputs["Vector"])
            m = _node(nt, "ShaderNodeMix", "lerp to decal", x + 100, y - 100)
            m.data_type = 'RGBA'
            lin = _node(nt, "ShaderNodeGamma", "decal to linear", x - 100, y - 250)
            lin.inputs["Gamma"].default_value = 2.2
            nt.links.new(t.outputs["Color"], lin.inputs["Color"])
            nt.links.new(t.outputs["Alpha"], m.inputs["Factor"])
            nt.links.new(final, _socket(m.inputs, "A_Color"))
            nt.links.new(lin.outputs[0], _socket(m.inputs, "B_Color"))
            final = _socket(m.outputs, "Result_Color")
    nt.links.new(final, base_in)

    # Additive: light added to what is behind.
    if additive and output.inputs["Surface"].is_linked:
        alpha_in = bsdf.inputs["Alpha"]
        _record_and_unlink(nt, output.inputs["Surface"].links[0], removed)
        emission = _node(nt, "ShaderNodeEmission", "additive light", output.location.x - 250, output.location.y - 150)
        if alpha_in.is_linked:
            scaled = _node(nt, "ShaderNodeVectorMath", "x alpha", output.location.x - 450, output.location.y - 150)
            scaled.operation = 'SCALE'
            nt.links.new(final, scaled.inputs[0])
            nt.links.new(alpha_in.links[0].from_socket, scaled.inputs["Scale"])
            nt.links.new(scaled.outputs[0], emission.inputs["Color"])
        else:
            nt.links.new(final, emission.inputs["Color"])
        clear = _node(nt, "ShaderNodeBsdfTransparent", "behind", output.location.x - 250, output.location.y - 300)
        add = _node(nt, "ShaderNodeAddShader", "add", output.location.x - 80, output.location.y - 200)
        nt.links.new(emission.outputs[0], add.inputs[0])
        nt.links.new(clear.outputs[0], add.inputs[1])
        nt.links.new(add.outputs[0], output.inputs["Surface"])
    material[RECORD] = json.dumps(removed)
    return True


def model_folder(scene):
    return str(scene["rasaModelFolder"]) if "rasaModelFolder" in scene.keys() else ""


def apply(scene, on):
    """Adds (on) or takes out the preview on every material of the scene's meshes; returns how many changed."""
    folder = model_folder(scene)
    done = set()
    count = 0
    for obj in scene.objects:
        if obj.type != 'MESH':
            continue
        uv_names = [u.name for u in obj.data.uv_layers]
        for slot in obj.material_slots:
            m = slot.material
            if m is None or m.name in done:
                continue
            done.add(m.name)
            if (build(m, uv_names, folder) if on else remove(m)):
                count += 1
    return count


def take_out_all():
    """Before a glTF export: every material in the file back to the importer's setup; returns the materials to restore."""
    restored = []
    for m in bpy.data.materials:
        if has_preview(m) and remove(m):
            restored.append(m.name)
    return restored


def _preview_changed(self, context):
    apply(context.scene, self.rasa_preview)


def register():
    bpy.types.Scene.rasa_preview = bpy.props.BoolProperty(
        name="Game-like preview", default=False, update=_preview_changed,
        description="Material nodes closer to the game's shaders (vertex colour x 2, hue, detail and decal maps, additive glow). Taken "
                    "out for every glTF export and put back after")


def unregister():
    del bpy.types.Scene.rasa_preview
