"""The editor's reading of scene documents (MissionValidation.cpp missionSceneReadError) against Rasa.NET's own deserialiser: takes
the World database's mission_scene_binding and mission_experience_binding documents, makes random one-change mutations of them
(a member renamed, added or removed, a value swapped for another JSON type or an edge-case number / enum / key, the "$kind"
discriminator moved or changed, ...), reads every one with this folder's harness (Rasa.Test.dll --read: System.Text.Json with
MissionContentCodec.Options) and with viewport_test (RASA_SCENEREAD), and reports every document where one reads it and the other
does not.
scene_fuzz.py <harness folder> <World database.db> <viewport_test.exe> [count per kind (default 3000)] [seed]"""
import json, os, random, sqlite3, subprocess, sys, tempfile

harness, database, viewport = sys.argv[1:4]
count = int(sys.argv[4]) if len(sys.argv) > 4 else 3000
rng = random.Random(int(sys.argv[5]) if len(sys.argv) > 5 else 1)
work = tempfile.mkdtemp(prefix='scene_fuzz_')

c = sqlite3.connect(database)
scenes = [json.loads(b) for (b,) in c.execute('select bindings from mission_scene_binding')]
experiences = [json.loads(b) for (b,) in c.execute('select bindings from mission_experience_binding')]

NUMBERS = ['0', '1', '-1', '-0', '1.5', '2.0', '1e2', '1E-1', '255', '256', '-128', '65535', '65536', '2147483647', '2147483648',
           '-2147483648', '-2147483649', '4294967295', '4294967296', '9223372036854775807', '18446744073709551616', '1e39', '1e309', '3']
STRINGS = ['""', '" "', '"x"', '"0"', '"1"', '"+1"', '"-1"', '" 2 "', '"01"', '"1.0"', '"true"', '"null"', '"Creature"', '"creature"',
           '"Object, Creature"', '"Creature,"', '",Creature"', '"1,Creature"', '"Completed"', '"Remove"', '"Personal"', '"NearbyParty"',
           '"level"', '"Level"', '"reward"', '"ensure"', '"WallClock"', '"accepted"', '"Choice"', '"2147483648"']
OTHERS = ['null', 'true', 'false', '[]', '{}', '[1]', '[0]', '[1,1]', '[[1]]', '[null]', '{"1":1}', '{"x":1}', '{"$kind":"level"}',
          '{"$kind":"all","items":[]}', '[{"$kind":"reward"}]', '{"x":1,"y":2,"z":3}']
VALUES = NUMBERS + STRINGS + OTHERS
KEYS = ['0', '1', '01', '+1', '-1', ' 1', 'x', '', '1.0', '4294967296', 'Accepted', 'accepted', 'Completed,Accepted', '2147483648',
        '-2147483649']


def paths(v, at=()):
    """Every (container, key) position in the document."""
    out = []
    if isinstance(v, dict):
        for k in list(v):
            out.append((v, k))
            out += paths(v[k], at + (k,))
    elif isinstance(v, list):
        for i in range(len(v)):
            out.append((v, i))
            out += paths(v[i], at + (i,))
    return out


def objects(v):
    out = []
    if isinstance(v, dict):
        out.append(v)
        for x in v.values():
            out += objects(x)
    elif isinstance(v, list):
        for x in v:
            out += objects(x)
    return out


def mutate(doc):
    d = json.loads(json.dumps(doc))
    spots = paths(d)
    objs = objects(d)
    kind = rng.randrange(9)
    if kind == 0 and spots:                                   # a value replaced (written later as raw JSON text)
        container, key = rng.choice(spots)
        container[key] = RawValue(rng.choice(VALUES))
    elif kind == 1 and objs:                                  # an unknown / differently-cased member added
        o = rng.choice(objs)
        names = list(o) or ['script']
        name = rng.choice(names)
        o[rng.choice([name.upper(), name[:1].upper() + name[1:], name + 'x', '$id', '$kind', 'bogus'])] = RawValue(rng.choice(VALUES))
    elif kind == 2 and objs:                                  # a member removed
        o = rng.choice([x for x in objs if x] or objs)
        if o:
            del o[rng.choice(list(o))]
    elif kind == 3 and objs:                                  # "$kind" moved after another member, or changed
        poly = [x for x in objs if '$kind' in x]
        if poly:
            o = rng.choice(poly)
            k = o.pop('$kind')
            if rng.random() < 0.5 and o:
                o['$kind'] = k                                # now last
            else:
                o['$kind'] = rng.choice(['level', 'Level', 'reward', 'ensure', 'bogus', 'all', 'not', 'objective'])
                o.update({n: o.pop(n) for n in [n for n in list(o) if n != '$kind']})   # kind first again
    elif kind == 4 and objs:                                  # a dictionary key changed
        o = rng.choice(objs)
        if o:
            k = rng.choice(list(o))
            o[rng.choice(KEYS)] = o.pop(k)
    elif kind == 5 and spots:                                 # a number nudged to an edge
        nums = [(cont, k) for cont, k in spots if isinstance(cont[k], (int, float)) and not isinstance(cont[k], bool)]
        if nums:
            cont, k = rng.choice(nums)
            cont[k] = RawValue(rng.choice(NUMBERS))
    elif kind == 6 and spots:                                 # a string swapped for an enum-ish edge case
        strs = [(cont, k) for cont, k in spots if isinstance(cont[k], str)]
        if strs:
            cont, k = rng.choice(strs)
            cont[k] = RawValue(rng.choice(STRINGS))
    elif kind == 7 and spots:                                 # a list element duplicated / emptied
        lists = [(cont, k) for cont, k in spots if isinstance(cont[k], list)]
        if lists:
            cont, k = rng.choice(lists)
            cont[k] = (cont[k] + cont[k][:1]) if rng.random() < 0.5 else []
    elif kind == 8 and spots:                                 # an object replaced by an empty one
        dicts = [(cont, k) for cont, k in spots if isinstance(cont[k], dict)]
        if dicts:
            cont, k = rng.choice(dicts)
            cont[k] = {}
    return d


class RawValue:
    def __init__(self, text):
        self.text = text


def dump(v):
    if isinstance(v, RawValue):
        return v.text
    if isinstance(v, dict):
        return '{' + ','.join(json.dumps(k) + ':' + dump(x) for k, x in v.items()) + '}'
    if isinstance(v, list):
        return '[' + ','.join(dump(x) for x in v) + ']'
    return json.dumps(v)


def run(kind, docs):
    lines = [dump(mutate(rng.choice(docs))) for _ in range(count)]
    path = os.path.join(work, kind + '.txt')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    net = subprocess.run(['dotnet', os.path.join(harness, 'Rasa.Test.dll'), '--read', kind, path], capture_output=True, text=True).stdout.splitlines()
    env = dict(os.environ, RASA_SCENEREAD=kind + ',' + path)
    cpp = subprocess.run([viewport], capture_output=True, text=True, env=env).stdout.splitlines()
    assert len(net) == len(cpp) == len(lines), (len(net), len(cpp), len(lines))
    bad = [(l, n, e) for l, n, e in zip(lines, net, cpp) if (n == 'ok') != (e == 'ok')]
    fails = sum(1 for n in net if n != 'ok')
    print(f'{kind}: {len(lines)} documents, {fails} the server cannot read, {len(bad)} disagree')
    for l, n, e in bad[:15]:
        print('  server:', n[:200])
        print('  editor:', e[:200])
        print('  doc:', l[:300])
    return len(bad)


bad = run('scene', scenes)
if experiences:
    bad += run('experience', experiences)
sys.exit(1 if bad else 0)
