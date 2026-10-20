"""The catalog's scene phase (MissionValidation.cpp validateCatalogScenes) against Rasa.NET's own MissionContentCatalog.Load: each case
puts one defect into a fresh copy of a World database, runs this folder's harness (Rasa.Test.dll: loader, validator, then the whole
catalog) and the editor's port (viewport_test RASA_MISSIONCHECK), and checks that the server's start-up failure message is among the
editor's start-up errors (the server stops at the first; the editor lists every one).
scene_defects.py <harness folder> <World database.db> <viewport_test.exe> <game root> [case ...]"""
import json, os, shutil, sqlite3, subprocess, sys, tempfile

harness, source, viewport, game = sys.argv[1:5]
only = set(sys.argv[5:])
viewport = os.path.abspath(viewport)
work = tempfile.mkdtemp(prefix='scene_defects_')


def enabled_revision(c, mission):
    return c.execute('select content_revision from mission_content_definition where mission_id=? and enabled=1', (mission,)).fetchone()[0]


def edit_document(c, mission, change):
    revision = enabled_revision(c, mission)
    document = json.loads(c.execute('select bindings from mission_scene_binding where mission_id=? and content_revision=?', (mission, revision)).fetchone()[0])
    change(document)
    c.execute('update mission_scene_binding set bindings=? where mission_id=? and content_revision=?', (json.dumps(document), mission, revision))


def copy_row(c, table, where, args, changes):
    columns = [r[1] for r in c.execute(f'pragma table_info("{table}")')]
    row = c.execute(f'select * from "{table}" where {where}', args).fetchone()
    values = dict(zip(columns, row))
    values.update(changes)
    c.execute(f'insert into "{table}" ({",".join(columns)}) values ({",".join("?" * len(columns))})', [values[k] for k in columns])


def no_scene_scenario(c):          # a scenario on a mission without a scene binding
    copy_row(c, 'mission_scenario', 'mission_id=428', (), {'mission_id': 1741, 'content_revision': enabled_revision(c, 1741)})


def shareable_scripted(c):         # a scripted assignment made shareable without party joining
    c.execute('update mission_content_definition set shareable=1 where mission_id=1407 and enabled=1')


def public_spawn_missing(c):       # the public encounter's spawn pool deleted
    c.execute('delete from spawnpool where id=630001')


def dialogue_branch(c):            # a choice topic's transition removed
    r = enabled_revision(c, 1390)
    for t in ('mission_trigger', 'mission_action', 'mission_objective_transition'):
        c.execute(f'delete from {t} where mission_id=1390 and content_revision=? and objective_id=1 and transition_id=2', (r,))


def radio_offer(c):                # the offered mission's Scene source changed to a server event
    r = enabled_revision(c, 701)
    sources = json.loads(c.execute('select radio_sources from mission_channel_policy where mission_id=701 and content_revision=?', (r,)).fetchone()[0])
    sources[0]['Kind'] = 0
    c.execute('update mission_channel_policy set radio_sources=? where mission_id=701 and content_revision=?', (json.dumps(sources), r))


def radio_requirement(c):          # a radio source requirement naming no handler
    r = enabled_revision(c, 2010)
    sources = json.loads(c.execute('select radio_sources from mission_channel_policy where mission_id=2010 and content_revision=?', (r,)).fetchone()[0])
    sources[0]['Requirement']['Key'] = 'character.nonsense'
    c.execute('update mission_channel_policy set radio_sources=? where mission_id=2010 and content_revision=?', (json.dumps(sources), r))


def experience_trigger(c):         # a mission the Bootcamp experience triggers on disabled
    c.execute('update mission_content_definition set enabled=0 where mission_id=1992')


def item_intent_key(c):            # an issue-mission-item action naming an item binding the scene lacks
    row = c.execute('select item_intent from mission_action where mission_id=1995 and objective_id=3 and transition_id=1 and action_id=4').fetchone()
    intent = json.loads(row[0])
    intent['itemKey'] = 'bombx'
    c.execute('update mission_action set item_intent=? where mission_id=1995 and objective_id=3 and transition_id=1 and action_id=4', (json.dumps(intent),))


def acceptance_quantity(c):        # an acceptance item above the binding's maximum
    edit_document(c, 2005, lambda d: d['acceptanceItems'][0].update(quantity=5))


def unknown_script(c):             # a script the server does not register (row and document agree)
    r = enabled_revision(c, 1994)
    edit_document(c, 1994, lambda d: d.update(script='bootcamp.nope'))
    c.execute('update mission_scene_binding set script_key=? where mission_id=1994 and content_revision=?', ('bootcamp.nope', r))


def inconsistent_script(c):        # the row's script_key differs from the document's
    c.execute('update mission_scene_binding set script_key=? where mission_id=1994 and content_revision=?', ('bootcamp.other', enabled_revision(c, 1994)))


def duplicate_operation(c):        # two sequences use one operation key
    def change(d):
        sequences = [s for s in d['sequences'].values() if s.get('world')]
        sequences[1]['world'][0]['operationKey'] = sequences[0]['world'][0]['operationKey']
    edit_document(c, 1994, change)


def timer_target(c):               # a timer naming a sequence that does not exist
    def change(d):
        for s in d['sequences'].values():
            for t in s.get('timers', []):
                t['sequenceId'] = 999
                return
    mission = next(m for m, b in c.execute('select mission_id, bindings from mission_scene_binding')
                   if any(s.get('timers') for s in json.loads(b)['sequences'].values()) and enabled_revision(c, m) is not None)
    edit_document(c, mission, change)


def credit_objective(c):           # an objective the scene's credit, title and requirement name deleted
    r = enabled_revision(c, 1449)
    for t in ('mission_trigger', 'mission_action', 'mission_objective_transition', 'mission_indicator', 'mission_objective_definition'):
        c.execute(f'delete from {t} where mission_id=1449 and content_revision=? and objective_id=7', (r,))


def object_conversation(c):        # an object conversation whose kind no topic offers
    def change(d):
        for a in d['actors'].values():
            if a.get('conversation'):
                a['conversation']['kind'] = 'Ambient'
                return
    mission = next(m for m, b in c.execute('select mission_id, bindings from mission_scene_binding')
                   if any(a.get('conversation') for a in json.loads(b)['actors'].values()))
    edit_document(c, mission, change)


def gameplay_policy(c):            # a tag with whitespace
    def change(d):
        for a in d['actors'].values():
            if a.get('gameplayPolicy'):
                a['gameplayPolicy']['tags'] = ['a a']
                return
    mission = next(m for m, b in c.execute('select mission_id, bindings from mission_scene_binding')
                   if any(a.get('gameplayPolicy') for a in json.loads(b)['actors'].values()))
    edit_document(c, mission, change)


def route_actor(c):                # a route run by an object actor
    def change(d):
        objects = [k for k, a in d['actors'].items() if a['kind'] == 'Object']
        for s in d['sequences'].values():
            for i in s.get('world', []):
                if i['$kind'] == 'route':
                    i['role'] = objects[0]
                    return
    mission = next(m for m, b in c.execute('select mission_id, bindings from mission_scene_binding')
                   if any(i['$kind'] == 'route' for s in json.loads(b)['sequences'].values() for i in s.get('world', []))
                   and any(a['kind'] == 'Object' for a in json.loads(b)['actors'].values()))
    edit_document(c, mission, change)


def item_drop_objective(c):        # the objective a mission item drop names loses its item-acquisition rule (event kind unknown)
    r = enabled_revision(c, 425)
    c.execute('update mission_trigger set event_kind=99 where mission_id=425 and content_revision=? and kind=2', (r,))


def action_intent_kind(c):         # an issue-mission-item action carrying a consume intent (an operational mission without a scene)
    r = enabled_revision(c, 442)
    row = c.execute('select objective_id, transition_id, action_id from mission_action where mission_id=442 and content_revision=? and kind=10', (r,)).fetchone()
    c.execute('update mission_action set item_intent=? where mission_id=442 and content_revision=? and objective_id=? and transition_id=? and action_id=?',
              ('{"$kind":"consume-mission-item","missionId":442,"itemKey":"x","quantity":1,"scope":"AssignmentIssued","operationKey":"defect"}', r, *row))


def radio_target_inactive(c):      # the mission a scene offers by radio becomes inactive (radio acceptance switched off)
    c.execute('update mission_channel_policy set acceptance_channel=1 where mission_id=701 and content_revision=?', (enabled_revision(c, 701),))


# The document itself unreadable by the server's deserialiser (MissionContentLoader / the catalog: System.Text.Json with
# MissionContentCodec.Options) - MissionValidation.cpp missionSceneReadError.
def scene_unmapped_member(c):      # a member the codec cannot map (unmapped members are disallowed)
    edit_document(c, 1994, lambda d: d.update(scriptVersion=1))


def scene_required_member(c):      # a mission item binding without its [JsonRequired] scope
    edit_document(c, 479, lambda d: d['items'][0].pop('scope'))


def scene_discriminator(c):        # a requirement whose "$kind" is not its first member
    def change(d):
        r = d['requirement']
        d['requirement'] = {k: v for k, v in r.items() if k != '$kind'}
        d['requirement']['$kind'] = r['$kind']
    edit_document(c, 1392, change)


def scene_value_type(c):           # a credit policy's mode as an unknown enum name
    def change(d):
        key = next(iter(d['credit']))
        d['credit'][key]['mode'] = 'Party'
    edit_document(c, 1449, change)


def experience_unmapped_member(c):  # the Bootcamp experience with a member its scene type lacks
    row = c.execute('select experience_key, bindings from mission_experience_binding').fetchone()
    document = json.loads(row[1])
    document['scene']['actors'][next(iter(document['scene']['actors']))]['facing'] = 0
    c.execute('update mission_experience_binding set bindings=? where experience_key=?', (json.dumps(document), row[0]))


cases = [scene_unmapped_member, scene_required_member, scene_discriminator, scene_value_type, experience_unmapped_member,
         item_drop_objective, action_intent_kind, radio_target_inactive, no_scene_scenario, shareable_scripted, public_spawn_missing, dialogue_branch, radio_offer, radio_requirement, experience_trigger,
         item_intent_key, acceptance_quantity, unknown_script, inconsistent_script, duplicate_operation, timer_target, credit_objective,
         object_conversation, gameplay_policy, route_actor]


def server_failure(db):
    out = subprocess.run(['dotnet', os.path.join(harness, 'Rasa.Test.dll'), db[:-3]], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if 'START-UP FAILS:' in line:
            message = line.split('START-UP FAILS:', 1)[1].strip()
            return message.split(': ', 1)[1] if message.split(': ', 1)[0].endswith('Exception') else message
    return None


def editor_failures(db):
    env = dict(os.environ, RASA_DB=db, RASA_MISSIONCHECK='1')
    out = subprocess.run([viewport, game, os.path.join(game, 'data', 'game.zip'), 'adv_bootcamp', os.path.join(work, 'm.rgba'), '64', '64',
                          '380', '122', '148', '380', '127', '159', 'hide'], capture_output=True, text=True, env=env).stdout
    return [line.split(' startup: ', 1)[1] for line in out.splitlines() if line.startswith('  mission ') and ' startup: ' in line]


passed = 0
for case in cases:
    if only and case.__name__ not in only:
        continue
    db = os.path.join(work, case.__name__ + '.db')
    shutil.copyfile(source, db)
    c = sqlite3.connect(db)
    case(c)
    c.commit()
    c.close()
    server = server_failure(db)
    editor = editor_failures(db)
    # The deserialiser's messages end in " Path: $... | LineNumber: ..."; the editor gives the path its own way, and words a value it
    # cannot convert by the value's type (the deserialiser names the record whose constructor takes it): either must match.
    head = server.split(' Path: ', 1)[0] if server else None
    path = server.split(' Path: ', 1)[1].split(' | ', 1)[0] if server and ' Path: ' in server else None
    ok = server is not None and any(head in e or (path and 'could not be converted' in head and f' at {path} ' in e) for e in editor)
    passed += ok
    print(f'{"PASS" if ok else "FAIL"} {case.__name__}')
    print(f'     server: {server}')
    for e in editor:
        print(f'     editor: {e}')
print(f'{passed} of {len([c for c in cases if not only or c.__name__ in only])} cases agree')
