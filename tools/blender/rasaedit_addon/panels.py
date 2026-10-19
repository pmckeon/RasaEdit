"""Stage 2: the RasaEdit data on materials, pieces (meshes) and contact points (empties) as Properties-editor panels.

RasaEdit writes that data as JSON text custom properties (rasaMaterial, rasaPiece, rasaContactPoint; Blender keeps strings verbatim,
see CLAUDE.md "Model exchange"). The panels edit property groups mirrored from it: every change writes the JSON straight back, so any
glTF export with custom properties carries it (the add-on's Export, or File > Export > glTF with Include > Custom Properties).

What RasaEdit's import reads from each (ModelImport.cpp):
- rasaMaterial: effect + parameters [key, type, value]; without it the material property rasaEffect (default TRDiffMapVertColor.fx)
  and the parameters of a shipped piece with that effect. DiffuseTexture / NormalMapTexture follow the material's Base Color /
  Normal Map images; the other texture slots name a game texture, or a PNG of that name in the export's textures folder.
- rasaPiece: group, name, swapId, userData (and index / bone / layout, kept as they are).
- rasaContactPoint: id and the key / value pairs (cpName, lsys, psys, light overrides ...); an empty named HPS_... etc. without it is
  a contact point too.
"""

import json

import bpy
from bpy.props import BoolProperty, CollectionProperty, EnumProperty, FloatVectorProperty, IntProperty, PointerProperty, StringProperty

from . import schema

_populating = False         # no write-back while the groups are filled from the JSON


def rasa_json(idblock, key):
    if idblock is None or key not in idblock.keys():
        return None
    value = idblock[key]
    if isinstance(value, str):
        try:
            return json.loads(value)
        except ValueError:
            return None
    try:
        return value.to_dict()
    except AttributeError:
        return value if isinstance(value, dict) else None


def _changed(self, context):
    if not _populating:
        write_back(self.id_data)


# ---- property groups ----


class RASAEDIT_PG_param(bpy.types.PropertyGroup):
    key: StringProperty(name="Key", update=_changed)
    ptype: IntProperty(name="Type", default=4)
    b: BoolProperty(name="Value", update=_changed)
    i: IntProperty(name="Value", update=_changed)
    count: IntProperty(name="Floats", min=0, max=16)
    f: FloatVectorProperty(name="Value", size=16, update=_changed)
    s: StringProperty(name="Value", update=_changed)


class RASAEDIT_PG_pair(bpy.types.PropertyGroup):
    key: StringProperty(name="Key", update=_changed)
    value: StringProperty(name="Value", update=_changed)


class RASAEDIT_PG_material(bpy.types.PropertyGroup):
    loaded: BoolProperty()
    effect: StringProperty(name="Effect", update=_changed)
    params: CollectionProperty(type=RASAEDIT_PG_param)


class RASAEDIT_PG_piece(bpy.types.PropertyGroup):
    loaded: BoolProperty()
    group: StringProperty(name="Group", description="The piece's GPCE group (material animation tracks address pieces by it)", update=_changed)
    piece_name: StringProperty(name="Name", update=_changed)
    swap_id: IntProperty(name="Swap set", min=0, description="Avatar swap set the piece belongs to (0 none, 99 the low-detail stand-in)",
                         update=_changed)
    user_data: CollectionProperty(type=RASAEDIT_PG_pair)


class RASAEDIT_PG_contact(bpy.types.PropertyGroup):
    loaded: BoolProperty()
    cp_id: IntProperty(name="Id", min=0, description="The contact point's id in the model (lsys / psys values name it before the comma)",
                       update=_changed)
    values: CollectionProperty(type=RASAEDIT_PG_pair)


# ---- JSON <-> groups ----


def param_value(p):
    if p.ptype == 1:
        return bool(p.b)
    if p.ptype == 2:
        return int(p.i)
    if p.ptype == 3:
        return [float(x) for x in list(p.f)[:p.count]]
    return p.s


def set_param(p, key, ptype, value):
    p.key = key
    p.ptype = int(ptype)
    if p.ptype == 1:
        p.b = bool(value)
    elif p.ptype == 2:
        p.i = int(value or 0)
    elif p.ptype == 3:
        floats = [float(x) for x in (value or [])][:16]
        p.count = len(floats)
        p.f = floats + [0.0] * (16 - len(floats))
    else:
        p.s = "" if value is None else str(value)


def write_back(idblock):
    if isinstance(idblock, bpy.types.Material):
        g = idblock.rasa
        if not g.loaded:
            return
        d = rasa_json(idblock, "rasaMaterial") or {}
        d["effect"] = g.effect
        d["parameters"] = [[p.key, p.ptype, param_value(p)] for p in g.params]
        idblock["rasaMaterial"] = json.dumps(d)
    elif isinstance(idblock, bpy.types.Mesh):
        g = idblock.rasa_piece
        if not g.loaded:
            return
        d = rasa_json(idblock, "rasaPiece") or {}
        d["group"] = g.group
        d["name"] = g.piece_name
        d["swapId"] = g.swap_id
        d["userData"] = [[u.key, u.value] for u in g.user_data]
        idblock["rasaPiece"] = json.dumps(d)
    elif isinstance(idblock, bpy.types.Object):
        g = idblock.rasa_cp
        if not g.loaded:
            return
        d = rasa_json(idblock, "rasaContactPoint") or {}
        d["id"] = g.cp_id
        d["values"] = [[v.key, v.value] for v in g.values]
        idblock["rasaContactPoint"] = json.dumps(d)


def populate(idblock):
    """Fills the block's group from its RasaEdit JSON (True when it had some)."""
    global _populating
    _populating = True
    try:
        if isinstance(idblock, bpy.types.Material):
            d = rasa_json(idblock, "rasaMaterial")
            if d is None:
                return False
            g = idblock.rasa
            g.params.clear()
            g.effect = d.get("effect", "")
            for entry in d.get("parameters", []):
                if len(entry) >= 3:
                    set_param(g.params.add(), entry[0], entry[1], entry[2])
            g.loaded = True
        elif isinstance(idblock, bpy.types.Mesh):
            d = rasa_json(idblock, "rasaPiece")
            if d is None:
                return False
            g = idblock.rasa_piece
            g.user_data.clear()
            g.group = d.get("group", "")
            g.piece_name = d.get("name", "")
            g.swap_id = int(d.get("swapId", 0))
            for pair in d.get("userData", []):
                if len(pair) == 2:
                    u = g.user_data.add()
                    u.key, u.value = str(pair[0]), str(pair[1])
            g.loaded = True
        elif isinstance(idblock, bpy.types.Object):
            d = rasa_json(idblock, "rasaContactPoint")
            if d is None:
                return False
            g = idblock.rasa_cp
            g.values.clear()
            g.cp_id = int(d.get("id", 0))
            for pair in d.get("values", []):
                if len(pair) == 2:
                    v = g.values.add()
                    v.key, v.value = str(pair[0]), str(pair[1])
            g.loaded = True
        else:
            return False
        return True
    finally:
        _populating = False


def populate_all(objects=None):
    """Fills every block with RasaEdit JSON (the imported objects, or the whole file); returns how many."""
    done = 0
    objects = list(objects) if objects is not None else list(bpy.data.objects)
    seen = set()
    for obj in objects:
        blocks = [obj]
        if obj.type == 'MESH':
            blocks.append(obj.data)
            blocks += [s.material for s in obj.material_slots if s.material]
        for b in blocks:
            if b.as_pointer() in seen:
                continue
            seen.add(b.as_pointer())
            if populate(b):
                done += 1
    return done


# ---- helpers for the panels ----


def contact_point_name(g):
    for v in g.values:
        if v.key.lower() == "cpname":
            return v
    return None


def split_cp_name(value):
    """(prefix, connection point name) of a cpName value, prefix "" when it has none of the eight."""
    prefixes = ("HPS_", "HPP_", "PPS_", "PPP_", "SPS_", "SPP_", "DPS_", "DPP_")
    if len(value) > 4 and value[:4].upper() in prefixes:
        return value[:4].upper(), value[4:]
    return "", value


def known_connection_point(data, name):
    return any(n.lower() == name.lower() for _, n in (data or {}).get("connectionPoints", []))


def is_texture(p):
    return p.ptype in (5, 8)


# ---- operators ----


def _owner_group(context, owner):
    if owner == "material":
        m = context.material
        return m.rasa if m else None
    if owner == "piece":
        mesh = context.object.data if context.object and context.object.type == 'MESH' else None
        return mesh.rasa_piece if mesh else None
    if owner == "cp":
        return context.object.rasa_cp if context.object else None
    return None


class RASAEDIT_OT_read_data(bpy.types.Operator):
    """Fill the RasaEdit panels from the RasaEdit data in this file (done by the import; needed after the JSON was edited by hand)"""
    bl_idname = "rasaedit.read_data"
    bl_label = "Read RasaEdit data"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        self.report({'INFO'}, "Read RasaEdit data of %d block(s)" % populate_all())
        return {'FINISHED'}


class RASAEDIT_OT_set_effect(bpy.types.Operator):
    """Choose the material's effect (the game's shader). Parameters the new effect's shipped pieces carry are added with their most
    common value, ones they never carry are removed, the rest keep their values"""
    bl_idname = "rasaedit.set_effect"
    bl_label = "Choose effect"
    bl_options = {'REGISTER', 'UNDO'}
    bl_property = "effect"

    def _items(self, context):
        return schema.enum_items(schema.load(context), "effects")

    effect: EnumProperty(name="Effect", items=_items)

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        material = context.material
        data = schema.load(context)
        if material is None or not self.effect:
            return {'CANCELLED'}
        g = material.rasa
        if not g.loaded:
            # A new material: RasaEdit takes the parameters of a shipped piece with this effect.
            material["rasaEffect"] = self.effect
            self.report({'INFO'}, "Effect %s (parameters from a shipped piece)" % self.effect)
            return {'FINISHED'}
        wanted = schema.effect_parameters(data, self.effect)
        if not wanted:
            g.effect = self.effect
            self.report({'WARNING'}, "%s is not in the schema: parameters left as they are" % self.effect)
            return {'FINISHED'}
        old = {p.key: (p.ptype, param_value(p)) for p in g.params}
        global _populating
        _populating = True
        try:
            g.params.clear()
            g.effect = self.effect
            for key, ptype, default, _count in wanted:
                value = old[key][1] if key in old and old[key][0] == ptype else default
                if key in ("DiffuseTexture", "NormalMapTexture") and key not in old:
                    value = ""          # filled from the material's image by RasaEdit
                set_param(g.params.add(), key, ptype, value)
        finally:
            _populating = False
        write_back(material)
        removed = [k for k in old if k not in {w[0] for w in wanted}]
        self.report({'INFO'}, "Effect %s%s" % (self.effect, (" - removed " + ", ".join(removed)) if removed else ""))
        return {'FINISHED'}


class RASAEDIT_OT_customise_material(bpy.types.Operator):
    """Give this material its own RasaEdit parameters (the effect's shipped defaults), to edit them here"""
    bl_idname = "rasaedit.customise_material"
    bl_label = "Edit the parameters"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        material = context.material
        data = schema.load(context)
        effect = material.get("rasaEffect", schema.DEFAULT_EFFECT)
        wanted = schema.effect_parameters(data, effect)
        if not wanted:
            self.report({'ERROR'}, "No schema entry for %s (write it with RasaEdit's Blender panel)" % effect)
            return {'CANCELLED'}
        global _populating
        _populating = True
        try:
            g = material.rasa
            g.params.clear()
            g.effect = effect
            for key, ptype, default, _count in wanted:
                set_param(g.params.add(), key, ptype, "" if key in ("DiffuseTexture", "NormalMapTexture") else default)
            g.loaded = True
        finally:
            _populating = False
        write_back(material)
        return {'FINISHED'}


class RASAEDIT_OT_add_missing(bpy.types.Operator):
    """Add the parameters the effect's shipped pieces carry that this material lacks (with their most common value)"""
    bl_idname = "rasaedit.add_missing_parameters"
    bl_label = "Add missing parameters"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        material = context.material
        g = material.rasa
        have = {p.key for p in g.params}
        added = 0
        for key, ptype, default, _count in schema.effect_parameters(schema.load(context), g.effect):
            if key not in have:
                set_param(g.params.add(), key, ptype, default)
                added += 1
        write_back(material)
        self.report({'INFO'}, "Added %d parameter(s)" % added)
        return {'FINISHED'}


class RASAEDIT_OT_remove_item(bpy.types.Operator):
    """Remove this entry"""
    bl_idname = "rasaedit.remove_item"
    bl_label = "Remove"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}
    owner: StringProperty()
    index: IntProperty()

    def execute(self, context):
        g = _owner_group(context, self.owner)
        if g is None:
            return {'CANCELLED'}
        items = g.params if self.owner == "material" else g.user_data if self.owner == "piece" else g.values
        if 0 <= self.index < len(items):
            items.remove(self.index)
            write_back(g.id_data)
        return {'FINISHED'}


class RASAEDIT_OT_add_pair(bpy.types.Operator):
    """Add a key / value pair"""
    bl_idname = "rasaedit.add_pair"
    bl_label = "Add"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}
    owner: StringProperty()
    key: StringProperty()
    value: StringProperty()

    def execute(self, context):
        g = _owner_group(context, self.owner)
        if g is None:
            return {'CANCELLED'}
        pair = (g.user_data if self.owner == "piece" else g.values).add()
        global _populating
        _populating = True
        pair.key, pair.value = self.key, self.value
        _populating = False
        write_back(g.id_data)
        return {'FINISHED'}


class RASAEDIT_OT_pick(bpy.types.Operator):
    """Choose from the names the game's files use"""
    bl_idname = "rasaedit.pick"
    bl_label = "Choose"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}
    bl_property = "item"
    owner: StringProperty()             # material / piece / cp
    field: StringProperty()             # param, group, swap, key, value, cpname
    index: IntProperty(default=-1)
    list_name: StringProperty()

    def _items(self, context):
        return schema.enum_items(schema.load(context), self.list_name)

    item: EnumProperty(name="Name", items=_items)

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        g = _owner_group(context, self.owner)
        if g is None or not self.item:
            return {'CANCELLED'}
        if self.field == "param":
            g.params[self.index].s = self.item
        elif self.field == "group":
            g.group = self.item
        elif self.field == "swap":
            g.swap_id = int(self.item)
        else:
            pairs = g.user_data if self.owner == "piece" else g.values
            pair = pairs[self.index]
            if self.field == "key":
                pair.key = self.item
            elif self.field == "cpname":
                prefix, _ = split_cp_name(pair.value)
                pair.value = (prefix or "HPS_") + self.item
            elif self.list_name in ("lightTemplates", "particleSystems"):
                # lsys / psys: "<contact point id>,<file>" (the id defaults to this contact point's).
                head = pair.value.split(",", 1)[0] if "," in pair.value else str(getattr(g, "cp_id", 1))
                pair.value = "%s,%s" % (head, self.item)
            else:
                pair.value = self.item
        return {'FINISHED'}


class RASAEDIT_OT_set_cp_prefix(bpy.types.Operator):
    """Socket (S) or plug (P) and the connection point type"""
    bl_idname = "rasaedit.set_cp_prefix"
    bl_label = "Connection point kind"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}
    index: IntProperty()
    prefix: EnumProperty(name="Kind", items=[
        ("HPS_", "HPS_ socket", "On a body: where special-FX events and accessories attach"),
        ("HPP_", "HPP_ plug", "On an accessory: the point that goes onto the body's socket"),
        ("PPS_", "PPS_", "type 2 socket"), ("PPP_", "PPP_", "type 2 plug"), ("SPS_", "SPS_", "type 3 socket"),
        ("SPP_", "SPP_", "type 3 plug"), ("DPS_", "DPS_", "type 4 socket"), ("DPP_", "DPP_", "type 4 plug")])

    def execute(self, context):
        g = context.object.rasa_cp
        pair = g.values[self.index]
        _, name = split_cp_name(pair.value)
        pair.value = self.prefix + name
        return {'FINISHED'}


class RASAEDIT_OT_add_piece_settings(bpy.types.Operator):
    """Give this mesh RasaEdit piece settings (group, name, swap set, user data); without them RasaEdit uses the mesh name"""
    bl_idname = "rasaedit.add_piece_settings"
    bl_label = "Add piece settings"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        mesh = context.object.data
        mesh["rasaPiece"] = json.dumps({"group": "", "name": mesh.name, "swapId": 0, "userData": []})
        populate(mesh)
        return {'FINISHED'}


class RASAEDIT_OT_make_contact_point(bpy.types.Operator):
    """Make this empty a contact point (a socket for effects, lights and accessories); parent it to a bone to follow it"""
    bl_idname = "rasaedit.make_contact_point"
    bl_label = "Make contact point"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        obj = context.object
        used = set()
        for o in context.scene.objects:
            d = rasa_json(o, "rasaContactPoint")
            if d is not None and o != obj:
                used.add(int(d.get("id", 0)))
        new_id = 0
        while new_id in used:
            new_id += 1
        obj["rasaContactPoint"] = json.dumps({"id": new_id, "values": [["cpName", "HPS_VFX1"]]})
        populate(obj)
        return {'FINISHED'}


# ---- panels ----


def schema_status(layout, context):
    data = schema.load(context)
    if data is None:
        box = layout.box()
        box.label(text="No RasaEdit schema found", icon='ERROR')
        box.label(text="Export a model from RasaEdit, or set its path in the add-on preferences")
    return data


class RASAEDIT_PT_material(bpy.types.Panel):
    bl_label = "Tabula Rasa material (RasaEdit)"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "material"

    @classmethod
    def poll(cls, context):
        return context.material is not None

    def draw(self, context):
        layout = self.layout
        material = context.material
        data = schema_status(layout, context)
        g = material.rasa
        if not g.loaded:
            if "rasaMaterial" in material.keys():
                layout.operator(RASAEDIT_OT_read_data.bl_idname, icon='FILE_REFRESH')
                return
            row = layout.row(align=True)
            row.label(text="Effect: %s" % material.get("rasaEffect", schema.DEFAULT_EFFECT + " (default)"))
            row.operator(RASAEDIT_OT_set_effect.bl_idname, text="", icon='VIEWZOOM')
            layout.label(text="New material: RasaEdit copies a shipped piece's parameters for this effect", icon='INFO')
            layout.operator(RASAEDIT_OT_customise_material.bl_idname)
            return
        row = layout.row(align=True)
        row.label(text="Effect: %s" % (g.effect or "(none)"))
        row.operator(RASAEDIT_OT_set_effect.bl_idname, text="", icon='VIEWZOOM')
        known = {p[0]: p for p in schema.effect_parameters(data, g.effect)} if data else {}
        if data and not known:
            layout.label(text="This effect is not used by any shipped model", icon='ERROR')
        missing = [k for k in known if k not in {p.key for p in g.params}]
        if missing:
            row = layout.row()
            row.label(text="%d parameter(s) of this effect missing" % len(missing), icon='INFO')
            row.operator(RASAEDIT_OT_add_missing.bl_idname, text="Add")
        col = layout.column(align=True)
        for index, p in enumerate(g.params):
            row = col.row(align=True)
            split = row.split(factor=0.4, align=True)
            label = split.row()
            unknown = bool(known) and p.key not in known
            label.label(text=p.key, icon='ERROR' if unknown else 'NONE')
            value = split.row(align=True)
            if p.ptype == 1:
                value.prop(p, "b", text="")
            elif p.ptype == 2:
                value.prop(p, "i", text="")
            elif p.ptype == 3:
                if p.count == 16:
                    value.label(text="4 x 4 matrix (below)")
                else:
                    for k in range(p.count):
                        value.prop(p, "f", index=k, text="")
            elif is_texture(p):
                if p.key in ("DiffuseTexture", "NormalMapTexture"):
                    value.label(text=("Base Color image" if p.key == "DiffuseTexture" else "Normal Map image") + (" / " + p.s if p.s else ""))
                else:
                    value.prop(p, "s", text="")
                    op = value.operator(RASAEDIT_OT_pick.bl_idname, text="", icon='VIEWZOOM')
                    op.owner, op.field, op.index, op.list_name = "material", "param", index, "textures"
            elif p.ptype == 4:
                value.prop(p, "s", text="")
            else:
                value.label(text="(raw data, kept)")
            rm = row.operator(RASAEDIT_OT_remove_item.bl_idname, text="", icon='X')
            rm.owner, rm.index = "material", index
            if p.ptype == 3 and p.count == 16:
                grid = col.grid_flow(columns=4, align=True)
                for k in range(16):
                    grid.prop(p, "f", index=k, text="")
        if any(is_texture(p) and p.key not in ("DiffuseTexture", "NormalMapTexture") for p in g.params):
            layout.label(text="Other texture slots: a game texture, or a PNG of that name in the export's textures folder", icon='INFO')


class RASAEDIT_PT_piece(bpy.types.Panel):
    bl_label = "Tabula Rasa piece (RasaEdit)"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "data"

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.type == 'MESH'

    def draw(self, context):
        from . import is_collision      # the collision rules live in the package
        layout = self.layout
        obj = context.object
        if is_collision(obj):
            layout.label(text="A collision mesh, not a piece", icon='INFO')
            return
        mesh = obj.data
        data = schema_status(layout, context)
        g = mesh.rasa_piece
        if not g.loaded:
            if "rasaPiece" in mesh.keys():
                layout.operator(RASAEDIT_OT_read_data.bl_idname, icon='FILE_REFRESH')
            else:
                layout.label(text="New piece: RasaEdit names it after the mesh", icon='INFO')
                layout.operator(RASAEDIT_OT_add_piece_settings.bl_idname)
            return
        d = rasa_json(mesh, "rasaPiece") or {}
        info = layout.column(align=True)
        if "index" in d:
            info.label(text="Piece %d of the original%s" % (d["index"], ", skinned" if d.get("skinned") else ""))
        else:
            info.label(text="New piece")
        if d.get("bone"):
            info.label(text="Bone: %s" % d["bone"])
        row = layout.row(align=True)
        row.prop(g, "group")
        op = row.operator(RASAEDIT_OT_pick.bl_idname, text="", icon='VIEWZOOM')
        op.owner, op.field, op.list_name = "piece", "group", "pieceGroups"
        layout.prop(g, "piece_name")
        row = layout.row(align=True)
        row.prop(g, "swap_id")
        names = dict((int(n), s) for n, s in (data or {}).get("swapSets", []))
        row.label(text=names.get(g.swap_id, "(not a known swap set)"))
        op = row.operator(RASAEDIT_OT_pick.bl_idname, text="", icon='VIEWZOOM')
        op.owner, op.field, op.list_name = "piece", "swap", "swapSets"
        layout.label(text="User data (LOD, LODPercentage, psys ...)")
        col = layout.column(align=True)
        for index, u in enumerate(g.user_data):
            row = col.row(align=True)
            row.prop(u, "key", text="")
            row.prop(u, "value", text="")
            if u.key.lower() == "psys":
                op = row.operator(RASAEDIT_OT_pick.bl_idname, text="", icon='VIEWZOOM')
                op.owner, op.field, op.index, op.list_name = "piece", "value", index, "particleSystems"
            rm = row.operator(RASAEDIT_OT_remove_item.bl_idname, text="", icon='X')
            rm.owner, rm.index = "piece", index
        row = layout.row(align=True)
        op = row.operator(RASAEDIT_OT_add_pair.bl_idname, text="Add")
        op.owner, op.key, op.value = "piece", "", ""
        op = row.operator(RASAEDIT_OT_add_pair.bl_idname, text="Add LOD")
        op.owner, op.key, op.value = "piece", "LOD", "1"


class RASAEDIT_PT_contact_point(bpy.types.Panel):
    bl_label = "Tabula Rasa contact point (RasaEdit)"
    bl_space_type = 'PROPERTIES'
    bl_region_type = 'WINDOW'
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.type == 'EMPTY'

    def draw(self, context):
        layout = self.layout
        obj = context.object
        data = schema_status(layout, context)
        g = obj.rasa_cp
        if not g.loaded:
            if "rasaContactPoint" in obj.keys():
                layout.operator(RASAEDIT_OT_read_data.bl_idname, icon='FILE_REFRESH')
            else:
                layout.label(text="Not a contact point", icon='INFO')
                layout.operator(RASAEDIT_OT_make_contact_point.bl_idname)
            return
        if obj.parent_type == 'BONE' and obj.parent_bone:
            layout.label(text="On bone %s" % obj.parent_bone)
        layout.prop(g, "cp_id")
        cp = contact_point_name(g)
        if cp is None:
            layout.label(text="No cpName: the engine cannot find it by connection point", icon='ERROR')
        else:
            prefix, name = split_cp_name(cp.value)
            index = list(g.values).index(cp)
            row = layout.row(align=True)
            row.label(text="Connection point")
            row.operator_menu_enum(RASAEDIT_OT_set_cp_prefix.bl_idname, "prefix", text=prefix or "(no prefix)").index = index
            op = row.operator(RASAEDIT_OT_pick.bl_idname, text=name or "(name)", icon='VIEWZOOM')
            op.owner, op.field, op.index, op.list_name = "cp", "cpname", index, "connectionPoints"
            if not prefix or (data and not known_connection_point(data, name)):
                layout.label(text="Not a name the engine knows: nothing finds this point by connection point", icon='ERROR')
        layout.label(text="Values (lights: lsys, then overrides between beginlight / endlight; particles: psys)")
        col = layout.column(align=True)
        for index, v in enumerate(g.values):
            row = col.row(align=True)
            row.prop(v, "key", text="")
            op = row.operator(RASAEDIT_OT_pick.bl_idname, text="", icon='VIEWZOOM')
            op.owner, op.field, op.index, op.list_name = "cp", "key", index, "contactPointKeys"
            row.prop(v, "value", text="")
            lists = {"lsys": "lightTemplates", "psys": "particleSystems"}
            if v.key.lower() in lists:
                op = row.operator(RASAEDIT_OT_pick.bl_idname, text="", icon='VIEWZOOM')
                op.owner, op.field, op.index, op.list_name = "cp", "value", index, lists[v.key.lower()]
            rm = row.operator(RASAEDIT_OT_remove_item.bl_idname, text="", icon='X')
            rm.owner, rm.index = "cp", index
        row = layout.row(align=True)
        op = row.operator(RASAEDIT_OT_add_pair.bl_idname, text="Add")
        op.owner, op.key, op.value = "cp", "", ""
        op = row.operator(RASAEDIT_OT_add_pair.bl_idname, text="Add light")
        op.owner, op.key, op.value = "cp", "lsys", "%d," % g.cp_id
        op = row.operator(RASAEDIT_OT_add_pair.bl_idname, text="Add particles")
        op.owner, op.key, op.value = "cp", "psys", "%d," % g.cp_id


classes = (RASAEDIT_PG_param, RASAEDIT_PG_pair, RASAEDIT_PG_material, RASAEDIT_PG_piece, RASAEDIT_PG_contact,
           RASAEDIT_OT_read_data, RASAEDIT_OT_set_effect, RASAEDIT_OT_customise_material, RASAEDIT_OT_add_missing,
           RASAEDIT_OT_remove_item, RASAEDIT_OT_add_pair, RASAEDIT_OT_pick, RASAEDIT_OT_set_cp_prefix, RASAEDIT_OT_add_piece_settings,
           RASAEDIT_OT_make_contact_point, RASAEDIT_PT_material, RASAEDIT_PT_piece, RASAEDIT_PT_contact_point)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Material.rasa = PointerProperty(type=RASAEDIT_PG_material)
    bpy.types.Mesh.rasa_piece = PointerProperty(type=RASAEDIT_PG_piece)
    bpy.types.Object.rasa_cp = PointerProperty(type=RASAEDIT_PG_contact)


def unregister():
    del bpy.types.Object.rasa_cp
    del bpy.types.Mesh.rasa_piece
    del bpy.types.Material.rasa
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
