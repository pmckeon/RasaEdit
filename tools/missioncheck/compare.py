"""Run Rasa.NET's validator (this folder's harness, built into <harness folder>) and the editor's port (viewport_test
RASA_MISSIONCHECK) on one World database; compare the diagnostics line by line.
compare.py <harness folder> <database.db> <viewport_test.exe> <game root>"""
import os, subprocess, sys, tempfile
harness, db, viewport, game = sys.argv[1:5]
viewport = os.path.abspath(viewport)
s = tempfile.gettempdir()
server = subprocess.run(['dotnet', os.path.join(harness, 'Rasa.Test.dll'), db[:-3]], capture_output=True, text=True).stdout
env = dict(os.environ, RASA_DB=db, RASA_MISSIONCHECK='1')
editor = subprocess.run([viewport, game, os.path.join(game, 'data', 'game.zip'), 'adv_bootcamp',
                         os.path.join(s, 'm.rgba'), '64', '64', '380', '122', '148', '380', '127', '159', 'hide'],
                        capture_output=True, text=True, env=env).stdout
def lines(text, head):
    summary = [l for l in text.splitlines() if l.startswith(head)]
    rows = [l.strip() for l in text.splitlines() if l.startswith('  mission ')]
    return (summary[0] if summary else '(no summary)'), rows
ss, sr = lines(server, 'server check')
es, er = lines(editor, 'mission check')
print('server:', ss)
print('editor:', es)
sset, eset = set(sr), set(er)
print(f'{len(sset & eset)} diagnostics identical; {len(sset - eset)} only from the server; {len(eset - sset)} only from the editor')
for l in sorted(sset - eset): print('  SERVER ONLY:', l)
for l in sorted(eset - sset): print('  EDITOR ONLY:', l)
