"""Stage 3: collision tools. A model's collision is one volume per bone (CLAUDE.md "Collision"): a walkable surface (any mesh), a box or
a sphere, in the bone's frame. RasaEdit's import reads meshes marked rasaCollision {bone, kind}: one box / sphere on a bone stays one
(fitted as the axis-aligned bounds in the bone's frame - a rotated box grows), anything else on a bone is merged into one surface;
without a bone it takes the bone the mesh is parented to, else bone 0.

Make collision (sidebar > RasaEdit, or Object > RasaEdit): from the selected meshes, as a convex hull, a copy, a simplified copy, a box
or a sphere - boxes and spheres fitted in the chosen bone's rest frame, so RasaEdit gets them exactly. Mark as collision: an existing
mesh. Both put the mesh into the Collision collection (excluded from the view by the import; RasaEdit exports it anyway)."""

import json

import bmesh
import bpy
import mathutils
from bpy.props import EnumProperty, FloatProperty

from . import panels

COLLECTION = "Collision"


def armature_of(objects):
    for o in objects:
        if o.type == 'ARMATURE':
            return o
        if o.parent and o.parent.type == 'ARMATURE':
            return o.parent
        for m in getattr(o, "modifiers", []):
            if m.type == 'ARMATURE' and m.object:
                return m.object
    return None


def bone_items(self, context):
    arm = armature_of(context.selected_objects or list(context.scene.objects)) or armature_of(context.scene.objects)
    items = [("", "(the mesh's own bone, else bone 0)", "")]
    if arm:
        items += [(b.name, b.name, "") for b in arm.data.bones]
    bone_items.cache = items          # Blender keeps no copy of dynamic enum items
    return items


def bone_frame(context, bone_name):
    """World matrix of the bone's rest frame (the frame RasaEdit fits boxes / spheres in), identity without one."""
    arm = armature_of(context.selected_objects) or armature_of(context.scene.objects)
    if arm and bone_name and bone_name in arm.data.bones:
        return arm.matrix_world @ arm.data.bones[bone_name].matrix_local
    return mathutils.Matrix.Identity(4)


def collision_collection(scene):
    for c in scene.collection.children_recursive:
        if c.name.split(".")[0] == COLLECTION:
            return c                     # the import's (excluded from the view; exported all the same)
    c = bpy.data.collections.new(COLLECTION)
    scene.collection.children.link(c)
    return c


def world_points(context, objects):
    deps = context.evaluated_depsgraph_get()
    points, triangles = [], []
    for o in objects:
        ev = o.evaluated_get(deps)
        mesh = ev.to_mesh()
        mesh.calc_loop_triangles()
        base = len(points)
        points += [o.matrix_world @ v.co for v in mesh.vertices]
        triangles += [tuple(base + i for i in t.vertices) for t in mesh.loop_triangles]
        ev.to_mesh_clear()
    return points, triangles


def mark(obj, bone, kind, scene):
    obj["rasaCollision"] = json.dumps({"bone": bone, "kind": kind})
    target = collision_collection(scene)
    for c in list(obj.users_collection):
        if c != target:
            c.objects.unlink(obj)
    if obj.name not in target.objects:
        target.objects.link(obj)


class RASAEDIT_OT_make_collision(bpy.types.Operator):
    """Make a collision mesh from the selected meshes (what players walk on and bump into)"""
    bl_idname = "rasaedit.make_collision"
    bl_label = "Make collision"
    bl_options = {'REGISTER', 'UNDO'}

    shape: EnumProperty(name="Shape", default='HULL', items=[
        ('HULL', "Convex hull", "The smallest convex shape around the selection (a walkable surface)"),
        ('COPY', "Copy", "The selection's own triangles (a walkable surface)"),
        ('SIMPLE', "Simplified copy", "The selection's triangles, reduced (a walkable surface)"),
        ('BOX', "Box", "A box around the selection, in the bone's frame"),
        ('SPHERE', "Sphere", "A sphere around the selection")])
    ratio: FloatProperty(name="Keep", default=0.25, min=0.01, max=1.0, subtype='FACTOR', description="Share of triangles a simplified copy keeps")
    bone: EnumProperty(name="Bone", items=bone_items)

    @classmethod
    def poll(cls, context):
        return any(o.type == 'MESH' for o in context.selected_objects)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        sources = [o for o in context.selected_objects if o.type == 'MESH']
        points, triangles = world_points(context, sources)
        if not points:
            self.report({'ERROR'}, "The selection has no vertices")
            return {'CANCELLED'}
        bone = self.bone or next((o.parent_bone for o in sources if o.parent_type == 'BONE' and o.parent_bone), "")
        frame = bone_frame(context, bone)
        inverse = frame.inverted()
        bm = bmesh.new()
        kind = "surface"
        if self.shape in ('BOX', 'SPHERE'):
            local = [inverse @ p for p in points]
            lo = mathutils.Vector((min(p.x for p in local), min(p.y for p in local), min(p.z for p in local)))
            hi = mathutils.Vector((max(p.x for p in local), max(p.y for p in local), max(p.z for p in local)))
            centre = (lo + hi) / 2
            if self.shape == 'BOX':
                kind = "box"
                bmesh.ops.create_cube(bm, size=1.0)
                for v in bm.verts:
                    v.co = centre + mathutils.Vector((v.co.x * (hi.x - lo.x), v.co.y * (hi.y - lo.y), v.co.z * (hi.z - lo.z)))
            else:
                kind = "sphere"
                radius = max((p - centre).length for p in local)
                bmesh.ops.create_uvsphere(bm, u_segments=16, v_segments=8, radius=max(radius, 1e-4))
                for v in bm.verts:
                    v.co += centre
            matrix = frame
        else:
            verts = [bm.verts.new(p) for p in points]
            if self.shape == 'HULL':
                bmesh.ops.convex_hull(bm, input=verts)
                bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
            else:
                for t in triangles:
                    try:
                        bm.faces.new([verts[i] for i in t])
                    except ValueError:
                        pass                      # a repeated triangle
                bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
            bmesh.ops.triangulate(bm, faces=bm.faces)
            matrix = mathutils.Matrix.Identity(4)
        mesh = bpy.data.meshes.new("collision %s" % (bone or "model"))
        bm.to_mesh(mesh)
        bm.free()
        obj = bpy.data.objects.new(mesh.name, mesh)
        obj.matrix_world = matrix
        context.scene.collection.objects.link(obj)
        if self.shape == 'SIMPLE' and self.ratio < 1.0:
            mod = obj.modifiers.new("simplify", 'DECIMATE')
            mod.ratio = self.ratio
            with context.temp_override(object=obj, active_object=obj, selected_objects=[obj]):
                bpy.ops.object.modifier_apply(modifier=mod.name)
        mark(obj, bone, kind, context.scene)
        count = len(obj.data.polygons)
        self.report({'INFO'}, "%s: %s with %d triangles on %s" % (obj.name, kind, count, bone or "the default bone"))
        if kind == "surface" and len(obj.data.vertices) > 65535:
            self.report({'WARNING'}, "over 65,535 vertices: RasaEdit leaves it out - simplify it")
        return {'FINISHED'}


class RASAEDIT_OT_mark_collision(bpy.types.Operator):
    """Mark the selected meshes as collision (moved into the Collision collection)"""
    bl_idname = "rasaedit.mark_collision"
    bl_label = "Mark as collision"
    bl_options = {'REGISTER', 'UNDO'}

    kind: EnumProperty(name="Kind", items=[
        ('surface', "Walkable surface", "The mesh as it is"),
        ('box', "Box", "RasaEdit fits the axis-aligned box around it in the bone's frame"),
        ('sphere', "Sphere", "RasaEdit fits the sphere around it")])
    bone: EnumProperty(name="Bone", items=bone_items)

    @classmethod
    def poll(cls, context):
        return any(o.type == 'MESH' for o in context.selected_objects)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        for o in [o for o in context.selected_objects if o.type == 'MESH']:
            bone = self.bone or (o.parent_bone if o.parent_type == 'BONE' else "")
            mark(o, bone, self.kind, context.scene)
        return {'FINISHED'}


def collision_notes(scene):
    """Per bone, what RasaEdit will make of the collision meshes (for the checks)."""
    from . import is_collision
    per_bone = {}
    for o in scene.objects:
        if o.type == 'MESH' and is_collision(o):
            d = panels.rasa_json(o, "rasaCollision") or panels.rasa_json(o.data, "rasaCollision") or {}
            bone = d.get("bone") or (o.parent_bone if o.parent_type == 'BONE' else "") or "(bone 0)"
            per_bone.setdefault(bone, []).append((o.name, d.get("kind", "surface")))
    notes = []
    for bone, meshes in per_bone.items():
        if len(meshes) > 1:
            notes.append("bone %s: %d collision meshes (%s) - merged into one walkable surface%s" % (
                bone, len(meshes), ", ".join(n for n, _ in meshes[:3]),
                " (their box / sphere kinds are lost)" if any(k != "surface" for _, k in meshes) else ""))
    return notes


classes = (RASAEDIT_OT_make_collision, RASAEDIT_OT_mark_collision)


def register():
    for c in classes:
        bpy.utils.register_class(c)


def unregister():
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
