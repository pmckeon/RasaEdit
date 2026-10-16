"""RasaEdit helper for Blender: Tabula Rasa models exchanged with RasaEdit as glTF.

Stage 1:
- glTF import hook (any glTF import while the add-on is enabled): a file written by RasaEdit (root node extras rasaModel) sets the
  scene to 30 fps before its animations are made (the game keys at 30 per second; Blender samples at the scene rate on export),
  puts low-detail pieces and collision meshes into excluded "Low detail" / "Collision" collections by their RasaEdit data (so they
  stay hidden on every trip, not only on the first), and records the skeleton's rest pose (rasaRestPose on the armature) for the
  shared-skeleton check.
- File > Import / Export > Tabula Rasa model (RasaEdit, .gltf): import with the settings RasaEdit expects; export as glTF Separate with
  custom properties, after checking the game's limits (Check for Tabula Rasa, also in the sidebar).
"""

bl_info = {
    "name": "RasaEdit Tabula Rasa models",
    "author": "RasaEdit",
    "version": (0, 1, 0),
    "blender": (4, 2, 0),
    "location": "File > Import / Export > Tabula Rasa model (RasaEdit); 3D view sidebar > RasaEdit",
    "description": "Import and export Tabula Rasa models exchanged with RasaEdit (glTF)",
    "category": "Import-Export",
}

import json

import bpy
from bpy.props import BoolProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

LOW_DETAIL = "Low detail"
COLLISION = "Collision"
GAME_FPS = 30
MAX_INFLUENCES = 3          # the game blends three bones per vertex
MAX_PALETTE = 28            # the largest shipped skinning palette (RasaEdit splits larger pieces)
MAX_COLLISION_VERTICES = 65535

# ---- RasaEdit data on Blender objects ----


def rasa_data(idblock, key):
    """A RasaEdit custom property (RasaEdit writes JSON text) as a dict, or None."""
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


def is_collision(obj):
    if obj.type != 'MESH':
        return False
    if rasa_data(obj, "rasaCollision") is not None or rasa_data(obj.data, "rasaCollision") is not None:
        return True
    return obj.name.lower().startswith("collision") or obj.data.name.lower().startswith("collision")


def is_low_detail(obj):
    """A piece the game draws at a distance (USDA LOD > 0) or a swap-set 99 stand-in."""
    if obj.type != 'MESH':
        return False
    piece = rasa_data(obj.data, "rasaPiece")
    if not piece:
        return False
    if piece.get("swapId") == 99:
        return True
    for pair in piece.get("userData", []):
        if len(pair) == 2 and str(pair[0]).lower() == "lod":
            try:
                return int(pair[1]) > 0
            except ValueError:
                return False
    return False


def layer_collection_of(layer, collection):
    if layer.collection == collection:
        return layer
    for child in layer.children:
        found = layer_collection_of(child, collection)
        if found:
            return found
    return None


def group_collection(name, parent):
    """The child collection `name` of `parent` (created when missing)."""
    for child in parent.children:
        if child.name == name or child.name.split(".")[0] == name:
            return child
    collection = bpy.data.collections.new(name)
    parent.children.link(collection)
    return collection


def move_to(obj, collection):
    for c in list(obj.users_collection):
        if c != collection:
            c.objects.unlink(obj)
    if obj.name not in collection.objects:
        collection.objects.link(obj)


def exclude(collection, view_layer):
    layer = layer_collection_of(view_layer.layer_collection, collection)
    if layer:
        layer.exclude = True


_pending_exclusions = []    # (scene name, collection name) to exclude after the current import


def apply_pending_exclusions():
    while _pending_exclusions:
        scene_name, collection_name = _pending_exclusions.pop()
        scene = bpy.data.scenes.get(scene_name)
        collection = bpy.data.collections.get(collection_name)
        if scene and collection:
            for view_layer in scene.view_layers:
                exclude(collection, view_layer)
    return None                 # a one-shot timer


def is_rasaedit_file(gltf):
    for node in gltf.data.nodes or []:
        extras = getattr(node, "extras", None)
        if isinstance(extras, dict) and "rasaModel" in extras:
            return True
    return False


# ---- glTF import hook ----


class glTF2ImportUserExtension:
    def __init__(self):
        self.rasaedit = False

    def gather_import_scene_before_hook(self, gltf_scene, blender_scene, gltf):
        self.rasaedit = is_rasaedit_file(gltf)
        if self.rasaedit and (blender_scene.render.fps != GAME_FPS or blender_scene.render.fps_base != 1.0):
            # Before the animations are made: their keys land on frames at this rate, and Blender's export samples every frame.
            blender_scene.render.fps = GAME_FPS
            blender_scene.render.fps_base = 1.0
            print("RasaEdit: scene set to %d fps (the game's key rate)" % GAME_FPS)

    def gather_import_scene_after_animation_hook(self, gltf_scene, blender_scene, gltf):
        objects = [v.blender_object for v in gltf.vnodes.values() if getattr(v, "blender_object", None) is not None]
        if not objects:
            return
        # The collection the model went into (the importer's scene collection), else the scene's.
        main = None
        for obj in objects:
            if obj.type in ('MESH', 'ARMATURE') and not is_low_detail(obj) and not is_collision(obj) and obj.users_collection:
                main = obj.users_collection[0]
                break
        if main is None:
            main = blender_scene.collection
        # Collections the importer made from RasaEdit's extra glTF scenes ("Low detail", "Collision") are reused; for files without
        # them (Blender's own re-exports put everything in one scene) the groups are made under the model's collection.
        existing = {}
        for collection in (getattr(gltf, "blender_collections", None) or {}).values():
            if collection is not None and collection.name.split(".")[0] in (LOW_DETAIL, COLLISION):
                existing[collection.name.split(".")[0]] = collection
        groups = {}
        for obj in objects:
            name = COLLISION if is_collision(obj) else LOW_DETAIL if is_low_detail(obj) else None
            if name is None:
                continue
            if name not in groups:
                groups[name] = existing.get(name) or group_collection(name, main)
            move_to(obj, groups[name])
        # RasaEdit's hidden-scene root empties ("Low detail", "Collision") follow their contents.
        for obj in objects:
            base = obj.name.split(".")[0]
            if obj.type == 'EMPTY' and base in (LOW_DETAIL, COLLISION) and base in groups:
                move_to(obj, groups[base])
        # Excluded once the import is over: the importer selects every object it made afterwards, which fails for objects in an
        # excluded collection. The Import command applies it at once; a timer covers File > Import > glTF.
        for collection in groups.values():
            _pending_exclusions.append((blender_scene.name, collection.name))
        if not bpy.app.background:
            bpy.app.timers.register(apply_pending_exclusions, first_interval=0.0)
        # The rest pose as imported, for the shared-skeleton check on export.
        for obj in objects:
            if obj.type == 'ARMATURE':
                rest = {b.name: [round(x, 6) for row in b.matrix_local for x in row] for b in obj.data.bones}
                obj["rasaRestPose"] = json.dumps(rest)


# ---- checks ----


def deform_bone_names(mesh_obj):
    arm = None
    for m in mesh_obj.modifiers:
        if m.type == 'ARMATURE' and m.object is not None:
            arm = m.object
    if arm is None:
        return None, set()
    return arm, {b.name for b in arm.data.bones if b.use_deform}


def check_scene(scene):
    """(errors, warnings, notes) for exporting `scene` to RasaEdit."""
    errors, warnings, notes = [], [], []
    objects = [o for o in scene.objects]
    root_data = None
    for o in objects:
        root_data = root_data or rasa_data(o, "rasaModel")
    if root_data is None:
        notes.append("no RasaEdit model data: RasaEdit imports this as a new model")
    has_actions = any(o.animation_data and (o.animation_data.action or o.animation_data.nla_tracks) for o in objects)
    if has_actions and (scene.render.fps != GAME_FPS or scene.render.fps_base != 1.0):
        warnings.append("scene runs at %.3g fps: animations are sampled at that rate (the game keys at %d - set the frame rate to %d)" %
                        (scene.render.fps / scene.render.fps_base, GAME_FPS, GAME_FPS))
    armatures = [o for o in objects if o.type == 'ARMATURE']
    if len(armatures) > 1:
        warnings.append("%d armatures: RasaEdit uses the first skin a mesh uses" % len(armatures))
    for o in objects:
        if o.type != 'MESH':
            continue
        if is_collision(o):
            if len(o.data.vertices) > MAX_COLLISION_VERTICES:
                errors.append("%s: %d vertices - a collision mesh may have at most %d (use a simpler one)" %
                              (o.name, len(o.data.vertices), MAX_COLLISION_VERTICES))
            data = rasa_data(o, "rasaCollision") or rasa_data(o.data, "rasaCollision") or {}
            if data.get("kind") == "box" and (abs(o.rotation_euler.x) > 1e-4 or abs(o.rotation_euler.y) > 1e-4 or abs(o.rotation_euler.z) > 1e-4):
                warnings.append("%s: a rotated collision box comes back as the larger axis-aligned box around it" % o.name)
            continue
        if not o.data.uv_layers:
            warnings.append("%s: no UV map (its textures have nothing to follow)" % o.name)
        for slot in o.material_slots:
            material = slot.material
            if material is None:
                notes.append("%s: an empty material slot - the default effect is used" % o.name)
            elif rasa_data(material, "rasaMaterial") is None and "rasaMaterial" in material.keys():
                errors.append("material %s: its rasaMaterial property is not valid JSON" % material.name)
            elif rasa_data(material, "rasaMaterial") is None:
                effect = material.get("rasaEffect", "TRDiffMapVertColor.fx")
                notes.append("material %s: new material, effect %s" % (material.name, effect))
        if "rasaPiece" in o.data.keys() and rasa_data(o.data, "rasaPiece") is None:
            errors.append("mesh %s: its rasaPiece property is not valid JSON" % o.data.name)
        arm, deform = deform_bone_names(o)
        if arm is not None:
            group_names = {g.index: g.name for g in o.vertex_groups}
            over, used = 0, set()
            for v in o.data.vertices:
                weights = [g for g in v.groups if g.weight > 1e-4 and group_names.get(g.group) in deform]
                if len(weights) > MAX_INFLUENCES:
                    over += 1
                for g in weights:
                    used.add(group_names[g.group])
            if over:
                warnings.append("%s: %d vertices have more than %d bone weights (the game keeps the %d largest)" % (o.name, over, MAX_INFLUENCES, MAX_INFLUENCES))
            if len(used) > MAX_PALETTE:
                notes.append("%s: %d bones - RasaEdit splits it into pieces of at most %d" % (o.name, len(used), MAX_PALETTE))
    for arm in armatures:
        recorded = None
        if "rasaRestPose" in arm.keys():
            try:
                recorded = json.loads(arm["rasaRestPose"])
            except ValueError:
                recorded = None
        if not recorded:
            continue
        moved = []
        for b in arm.data.bones:
            before = recorded.get(b.name)
            now = [x for row in b.matrix_local for x in row]
            if before is None:
                moved.append(b.name + " (new)")
            elif max(abs(x - y) for x, y in zip(now, before)) > 1e-4:
                moved.append(b.name)
        if moved:
            warnings.append("%s: %d bone(s) changed their rest pose (%s%s). A shared skeleton's other animations follow the original bones - "
                            "check this is meant" % (arm.name, len(moved), ", ".join(moved[:4]), "..." if len(moved) > 4 else ""))
    return errors, warnings, notes


# ---- operators ----


class RASAEDIT_OT_check(bpy.types.Operator):
    """Check the scene against the game's limits before exporting to RasaEdit"""
    bl_idname = "rasaedit.check"
    bl_label = "Check for Tabula Rasa"

    def execute(self, context):
        errors, warnings, notes = check_scene(context.scene)
        context.scene["rasaCheck"] = json.dumps({"errors": errors, "warnings": warnings, "notes": notes})
        for line in errors:
            self.report({'ERROR'}, line)
        for line in warnings:
            self.report({'WARNING'}, line)
        for line in notes:
            self.report({'INFO'}, line)
        if not errors and not warnings:
            self.report({'INFO'}, "Ready for RasaEdit (%d note(s))" % len(notes))
        return {'FINISHED'}


class RASAEDIT_OT_export(bpy.types.Operator, ExportHelper):
    """Export the scene for RasaEdit: glTF Separate with custom properties, after the game's checks"""
    bl_idname = "rasaedit.export_gltf"
    bl_label = "Export Tabula Rasa model (RasaEdit)"
    filename_ext = ".gltf"
    filter_glob: StringProperty(default="*.gltf", options={'HIDDEN'})
    export_anyway: BoolProperty(name="Export despite errors", default=False)

    def execute(self, context):
        errors, warnings, notes = check_scene(context.scene)
        context.scene["rasaCheck"] = json.dumps({"errors": errors, "warnings": warnings, "notes": notes})
        for line in warnings:
            self.report({'WARNING'}, line)
        if errors and not self.export_anyway:
            for line in errors:
                self.report({'ERROR'}, line)
            return {'CANCELLED'}
        # Every collection is exported (hidden Low detail / Collision included); custom properties carry RasaEdit's data.
        bpy.ops.export_scene.gltf(filepath=self.filepath, export_format='GLTF_SEPARATE', export_extras=True, export_animations=True,
                                  export_skins=True, export_yup=True, use_selection=False, use_visible=False, use_renderable=False,
                                  use_active_collection=False)
        self.report({'INFO'}, "Exported %s (%d warning(s))" % (self.filepath, len(warnings)))
        return {'FINISHED'}


class RASAEDIT_OT_import(bpy.types.Operator, ImportHelper):
    """Import a model RasaEdit exported (low-detail pieces and collision in hidden collections)"""
    bl_idname = "rasaedit.import_gltf"
    bl_label = "Import Tabula Rasa model (RasaEdit)"
    filename_ext = ".gltf"
    filter_glob: StringProperty(default="*.gltf;*.glb", options={'HIDDEN'})

    def execute(self, context):
        bpy.ops.import_scene.gltf(filepath=self.filepath, import_scene_as_collection=True)
        apply_pending_exclusions()
        return {'FINISHED'}


class RASAEDIT_PT_panel(bpy.types.Panel):
    bl_label = "RasaEdit"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "RasaEdit"

    def draw(self, context):
        layout = self.layout
        layout.operator(RASAEDIT_OT_import.bl_idname, text="Import model")
        layout.operator(RASAEDIT_OT_check.bl_idname, text="Check")
        layout.operator(RASAEDIT_OT_export.bl_idname, text="Export model")
        scene = context.scene
        layout.label(text="Frame rate: %.3g fps%s" % (scene.render.fps / scene.render.fps_base,
                                                      "" if scene.render.fps == GAME_FPS else " (game: %d)" % GAME_FPS))
        if "rasaCheck" in scene.keys():
            try:
                result = json.loads(scene["rasaCheck"])
            except ValueError:
                result = {}
            box = layout.box()
            for kind, icon in (("errors", 'ERROR'), ("warnings", 'INFO'), ("notes", 'DOT')):
                for line in result.get(kind, []):
                    box.label(text=line, icon=icon)
            if not any(result.get(k) for k in ("errors", "warnings", "notes")):
                box.label(text="No problems found", icon='CHECKMARK')


def menu_import(self, context):
    self.layout.operator(RASAEDIT_OT_import.bl_idname, text="Tabula Rasa model (RasaEdit, .gltf)")


def menu_export(self, context):
    self.layout.operator(RASAEDIT_OT_export.bl_idname, text="Tabula Rasa model (RasaEdit, .gltf)")


classes = (RASAEDIT_OT_check, RASAEDIT_OT_export, RASAEDIT_OT_import, RASAEDIT_PT_panel)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
