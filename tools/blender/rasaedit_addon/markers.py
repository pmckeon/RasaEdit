"""Stage 3: animation markers (<anim>.mkr: Footstep1, Strike, Windup1 ... at milliseconds; CLAUDE.md "Animation markers").

RasaEdit's export lists an animation's markers in its rasaAnimation entry (the root object's rasaModel.animations) as [type, ms, name].
The import turns them into the action's pose markers (Dope Sheet > Action Editor, View > Show Pose Markers), named by the engine's
type names; every glTF export (pre_export_hook) writes the action's pose markers back into that entry, and RasaEdit's import writes
<anim>.mkr when they differ from the game's. A marker keeps its exact milliseconds while it stays on its frame (30 fps frames are
33.3 ms apart); a moved or new one gets its frame's time. Only animations RasaEdit exported carry markers (a new action has no entry)."""

import json

import bpy
from bpy.props import EnumProperty

from . import panels, schema

RECORD = "rasaMarkers"          # on the action: the markers as imported, [[type, ms]]


def model_root():
    for o in bpy.data.objects:
        if "rasaModel" in o.keys():
            return o
    return None


def type_names(data):
    names = {int(t): n for t, n in (data or {}).get("markerTypes", [])}
    return names


def action_for(name):
    if name in bpy.data.actions:
        return bpy.data.actions[name]
    for a in bpy.data.actions:
        if a.name.split(".")[0] == name or a.name.startswith(name + "_"):
            return a
    return None


def import_markers(scene):
    """After an import: the model's markers onto its actions' pose markers; returns how many."""
    root = model_root()
    model = panels.rasa_json(root, "rasaModel") if root else None
    if not model:
        return 0
    fps = scene.render.fps / scene.render.fps_base
    count = 0
    for entry in model.get("animations", []):
        markers = entry.get("markers")
        action = action_for(entry.get("name", ""))
        if markers is None or action is None:
            continue
        for m in list(action.pose_markers):
            action.pose_markers.remove(m)
        for e in markers:
            if len(e) < 2:
                continue
            pm = action.pose_markers.new(e[2] if len(e) > 2 and e[2] != "unknown" else "#%d" % e[0])
            pm.frame = int(round(e[1] / 1000.0 * fps))
            count += 1
        action[RECORD] = json.dumps([[int(e[0]), int(e[1])] for e in markers if len(e) >= 2])
    return count


def marker_type(name, names):
    if name.startswith("#"):
        try:
            return int(name[1:])
        except ValueError:
            return None
    base = name.split(".")[0]           # Blender adds .001 to repeated names
    for t, n in names.items():
        if n.lower() == base.lower():
            return t
    return None


def export_markers(scene):
    """Before a glTF export: every action's pose markers into its rasaAnimation entry. Returns (written, problems)."""
    root = model_root()
    model = panels.rasa_json(root, "rasaModel") if root else None
    if not model:
        return 0, []
    names = type_names(schema.load(bpy.context))
    fps = scene.render.fps / scene.render.fps_base
    written, problems = 0, []
    entries = {e.get("name", ""): e for e in model.get("animations", [])}
    for action in bpy.data.actions:
        entry = entries.get(action.name) or entries.get(action.name.split(".")[0])
        if entry is None:
            if len(action.pose_markers):
                problems.append("%s: markers on an action RasaEdit did not export are not carried" % action.name)
            continue
        if "markers" not in entry and not len(action.pose_markers):
            continue
        try:
            recorded = json.loads(action[RECORD]) if RECORD in action.keys() else []
        except ValueError:
            recorded = []
        out = []
        for pm in sorted(action.pose_markers, key=lambda m: m.frame):
            t = marker_type(pm.name, names)
            if t is None:
                problems.append("%s: marker %s at frame %d is not a marker type - left out" % (action.name, pm.name, pm.frame))
                continue
            ms = None
            for i, (rt, rms) in enumerate(recorded):
                if rt == t and int(round(rms / 1000.0 * fps)) == pm.frame:
                    ms = rms
                    recorded.pop(i)
                    break
            if ms is None:
                ms = int(round(pm.frame / fps * 1000.0))
            out.append([t, max(0, ms), names.get(t, "unknown")])
        entry["markers"] = out
        written += len(out)
    root["rasaModel"] = json.dumps(model)
    return written, problems


class RASAEDIT_OT_add_marker(bpy.types.Operator):
    """Add an animation marker of a game type at the current frame to the active object's action"""
    bl_idname = "rasaedit.add_marker"
    bl_label = "Add animation marker"
    bl_options = {'REGISTER', 'UNDO'}
    bl_property = "kind"

    def _items(self, context):
        data = schema.load(context)
        items = [(n, n, "type %d" % t) for t, n in sorted(type_names(data).items())]
        RASAEDIT_OT_add_marker.cache = items or [("", "(no RasaEdit schema found)", "")]
        return RASAEDIT_OT_add_marker.cache

    kind: EnumProperty(name="Marker", items=_items)

    @classmethod
    def poll(cls, context):
        obj = context.object
        return obj is not None and obj.animation_data is not None and obj.animation_data.action is not None

    def invoke(self, context, event):
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        if not self.kind:
            return {'CANCELLED'}
        action = context.object.animation_data.action
        action.pose_markers.new(self.kind).frame = context.scene.frame_current
        return {'FINISHED'}


classes = (RASAEDIT_OT_add_marker,)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
