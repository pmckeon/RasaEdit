"""Headless check of the add-on's panel draw code (no UI in background mode): each panel's draw() runs against a mock layout that
records the calls and checks that every prop() names a real property and every operator() a registered operator.
blender --background --factory-startup --python test_addon_draw.py -- <RasaEdit export .gltf>"""
import bpy, sys, os, addon_utils

args = sys.argv[sys.argv.index('--') + 1:]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
bpy.ops.wm.read_factory_settings(use_empty=True)
addon_utils.enable("rasaedit_addon", default_set=True)
bpy.ops.rasaedit.import_gltf(filepath=args[0], game_preview=True)
problems = []


class Props:
    """Operator properties set by the panel (any name accepted, recorded)."""
    def __setattr__(self, k, v):
        object.__setattr__(self, k, v)


class Layout:
    def __init__(self):
        self.calls = 0

    def _child(self, *a, **k):
        return self

    row = column = box = split = grid_flow = _child

    def label(self, **k):
        self.calls += 1

    def separator(self, **k):
        pass

    def prop(self, data, name, **k):
        self.calls += 1
        if name not in data.bl_rna.properties:
            problems.append("prop %s on %s" % (name, data))

    def operator(self, idname, **k):
        self.calls += 1
        module, _, op = idname.partition(".")
        if not hasattr(getattr(bpy.ops, module), op) or getattr(getattr(bpy.ops, module), op).idname_py() is None:
            problems.append("operator %s" % idname)
        return Props()

    def operator_menu_enum(self, idname, prop, **k):
        return self.operator(idname)


from rasaedit_addon import panels, RASAEDIT_PT_panel
scene = bpy.context.scene
cases = []
for obj in scene.objects:
    if obj.type == 'MESH':
        for slot in obj.material_slots:
            if slot.material:
                cases.append((panels.RASAEDIT_PT_material, dict(object=obj, material=slot.material)))
        cases.append((panels.RASAEDIT_PT_piece, dict(object=obj)))
    if obj.type == 'EMPTY':
        cases.append((panels.RASAEDIT_PT_contact_point, dict(object=obj)))
# A new material and a new empty too (the "not RasaEdit data yet" branches).
fresh = bpy.data.materials.new("fresh")
fresh.use_nodes = True
cases.append((panels.RASAEDIT_PT_material, dict(material=fresh)))
empty = bpy.data.objects.new("plain empty", None)
scene.collection.objects.link(empty)
cases.append((panels.RASAEDIT_PT_contact_point, dict(object=empty)))
cases.append((RASAEDIT_PT_panel, dict()))
drawn = 0
for cls, ctx in cases:
    with bpy.context.temp_override(**ctx):
        if hasattr(cls, "poll") and not cls.poll(bpy.context):
            continue
        layout = Layout()
        try:
            cls.draw(type("P", (), {"layout": layout})(), bpy.context)
            drawn += 1
        except Exception as e:
            import traceback
            problems.append("%s: %s" % (cls.__name__, traceback.format_exc().strip().splitlines()[-1]))
print("DRAW %d panel draws, problems: %s" % (drawn, problems or "none"))
