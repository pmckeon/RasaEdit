"""Packs tools/blender/rasaedit_addon into rasaedit_addon.zip (beside this script) for Blender:
Edit > Preferences > Add-ons > (menu) Install from Disk..., then enable "RasaEdit Tabula Rasa models".
python make_addon_zip.py [out.zip]"""
import os, sys, zipfile

here = os.path.dirname(os.path.abspath(__file__))
source = os.path.join(here, "rasaedit_addon")
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "rasaedit_addon.zip")
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(source)):
        path = os.path.join(source, name)
        if os.path.isfile(path) and not name.endswith(".pyc"):
            # Extension format: the manifest at the zip's top level.
            z.write(path, name)
print("wrote", out)
