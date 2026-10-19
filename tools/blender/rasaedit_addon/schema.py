"""The schema RasaEdit writes for this add-on (rasaedit_schema.json, ModelExchange.cpp writeBlenderSchema): every effect the shipped
models use with its parameters (type, the most common value, how many pieces carry it), the connection point names, the keys pieces
and contact points use, the light / particle / texture names they can name, and the limits.

Where it is looked for, first found wins: the path recorded at import (the exported file's folder or the one above it - RasaEdit writes
it beside the model folders), the add-on preference, <home>/Documents/RasaEdit/Blender (RasaEdit's default export folder)."""

import json
import os

FILE_NAME = "rasaedit_schema.json"
DEFAULT_EFFECT = "TRDiffMapVertColor.fx"      # RasaEdit's default for a material without RasaEdit data
TYPE_NAMES = {1: "bool", 2: "int", 3: "floats", 4: "string", 5: "texture", 8: "texture"}

_cache = {"path": None, "mtime": None, "data": None, "index": None}
_enum_items = {}                                # list name -> enum items (kept alive: Blender keeps no copy)


def default_folder():
    return os.path.join(os.path.expanduser("~"), "Documents", "RasaEdit", "Blender")


def find_beside(gltf_path):
    """The schema in the exported file's folder or the one above it, or None."""
    folder = os.path.dirname(os.path.abspath(gltf_path))
    for candidate in (os.path.join(folder, FILE_NAME), os.path.join(os.path.dirname(folder), FILE_NAME)):
        if os.path.isfile(candidate):
            return candidate
    return None


def preference_path(context):
    addon = context.preferences.addons.get(__package__) if context else None
    return addon.preferences.schema_path if addon and getattr(addon, "preferences", None) else ""


def candidate_paths(context):
    paths = []
    scene = getattr(context, "scene", None)
    if scene is not None and "rasaSchemaPath" in scene.keys():
        paths.append(str(scene["rasaSchemaPath"]))
    pref = preference_path(context)
    if pref:
        paths.append(bpy_abspath(pref))
    paths.append(os.path.join(default_folder(), FILE_NAME))
    return paths


def bpy_abspath(path):
    try:
        import bpy
        return bpy.path.abspath(path)
    except Exception:
        return path


def load(context):
    """The schema as a dict, or None when none is found (cached; re-read when the file changes)."""
    for path in candidate_paths(context):
        try:
            mtime = os.path.getmtime(path)
        except OSError:
            continue
        if _cache["path"] == path and _cache["mtime"] == mtime:
            return _cache["data"]
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        _cache.update(path=path, mtime=mtime, data=data, index=None)
        _enum_items.clear()
        return data
    return None


def loaded_path():
    return _cache["path"]


def effect_index(data):
    """Effects by lower-case name."""
    if data is None:
        return {}
    if _cache["index"] is None or _cache["data"] is not data:
        _cache["index"] = {e["name"].lower(): e for e in data.get("effects", [])}
    return _cache["index"]


def effect(data, name):
    return effect_index(data).get((name or "").lower())


def effect_parameters(data, name):
    """[(key, type, default value, pieces)] the effect's shipped pieces carry, in their order."""
    e = effect(data, name)
    return [tuple(p) for p in e.get("parameters", [])] if e else []


def enum_items(data, list_name):
    """Enum items for a searchable list of the schema: effects, textures, lightTemplates, particleSystems, pieceGroups,
    contactPointKeys, connectionPoints, swapSets."""
    if list_name in _enum_items:
        return _enum_items[list_name]
    items = []
    if data is not None:
        if list_name == "effects":
            for e in data.get("effects", []):
                items.append((e["name"], e["name"], "%d shipped pieces (%d skinned)" % (e.get("pieces", 0), e.get("skinned", 0))))
        elif list_name == "connectionPoints":
            for number, name in data.get(list_name, []):         # chosen by name: cpName values spell it out
                items.append((name, name, "connection point %d" % number))
        elif list_name == "swapSets":
            for number, name in data.get(list_name, []):
                items.append((str(number), "%d %s" % (number, name), ""))
        elif list_name in ("pieceGroups", "contactPointKeys"):
            for name, count in data.get(list_name, []):
                items.append((name, name, "used %d times in the shipped models" % count))
        else:
            for name in data.get(list_name, []):
                items.append((name, name, ""))
    if not items:
        items = [("", "(no RasaEdit schema found)", "")]
    _enum_items[list_name] = items
    return items
