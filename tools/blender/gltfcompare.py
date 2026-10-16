"""Compare joints (TRS, inverse binds), meshes and animations of two glTF files (RasaEdit's export vs Blender's re-export).
gltfcompare.py <a.gltf> <b.gltf>"""
import json, struct, sys, os, math

def load(path):
    g = json.load(open(path))
    bins = [open(os.path.join(os.path.dirname(path), b['uri']), 'rb').read() for b in g['buffers']]
    return g, bins

def accessor(g, bins, i):
    a = g['accessors'][i]; v = g['bufferViews'][a['bufferView']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
    fmt = {5126: 'f', 5123: 'H', 5125: 'I', 5121: 'B', 5120: 'b', 5122: 'h'}[a['componentType']]
    size = struct.calcsize(fmt)
    stride = v.get('byteStride', n * size)
    base = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    data = bins[v['buffer']]
    out = []
    for k in range(a['count']):
        out.append(struct.unpack_from('<' + fmt * n, data, base + k * stride))
    return out

def joints(g, bins):
    skin = g['skins'][0]
    ibm = accessor(g, bins, skin['inverseBindMatrices'])
    out = {}
    for k, j in enumerate(skin['joints']):
        n = g['nodes'][j]
        out[n['name']] = (n.get('translation', [0, 0, 0]), n.get('rotation', [0, 0, 0, 1]), n.get('scale', [1, 1, 1]), ibm[k])
    return out

ga, ba = load(sys.argv[1]); gb, bb = load(sys.argv[2])
ja, jb = joints(ga, ba), joints(gb, bb)
print('joints', len(ja), len(jb), 'missing in b', [n for n in ja if n not in jb][:5], 'extra in b', [n for n in jb if n not in ja][:5])
worst = {'t': 0, 'r': 0, 's': 0, 'ibm': 0}
worstName = {}
for name, (t, r, s, m) in ja.items():
    if name not in jb: continue
    t2, r2, s2, m2 = jb[name]
    dt = max(abs(x - y) for x, y in zip(t, t2))
    dot = abs(sum(x * y for x, y in zip(r, r2)))
    dr = math.degrees(2 * math.acos(min(1, dot)))
    ds = max(abs(x - y) for x, y in zip(s, s2))
    dm = max(abs(x - y) for x, y in zip(m, m2))
    for k, v in (('t', dt), ('r', dr), ('s', ds), ('ibm', dm)):
        if v > worst[k]: worst[k] = v; worstName[k] = name
print('largest joint differences: translation %.6f m, rotation %.4f deg, scale %.6f, inverse bind %.6f' % (worst['t'], worst['r'], worst['s'], worst['ibm']), worstName)
# joint order
order_a = [ga['nodes'][j]['name'] for j in ga['skins'][0]['joints']]
order_b = [gb['nodes'][j]['name'] for j in gb['skins'][0]['joints']]
print('joint order same:', order_a == order_b)
print('meshes', len(ga.get('meshes', [])), len(gb.get('meshes', [])), 'primitives', sum(len(m['primitives']) for m in ga['meshes']), sum(len(m['primitives']) for m in gb['meshes']))
for m in gb['meshes'][:3]:
    p = m['primitives'][0]
    print('  b mesh', m.get('name'), 'attributes', sorted(p['attributes']), 'extras' in m, list(m.get('extras', {}).keys()))
print('vertices', sum(ga['accessors'][p['attributes']['POSITION']]['count'] for m in ga['meshes'] for p in m['primitives']),
      sum(gb['accessors'][p['attributes']['POSITION']]['count'] for m in gb['meshes'] for p in m['primitives']))
for a in gb.get('animations', [])[:2]:
    print('b animation', a.get('name'), 'channels', len(a['channels']), 'extras', list(a.get('extras', {}).keys()))
for a in ga.get('animations', [])[:2]:
    print('a animation', a.get('name'), 'channels', len(a['channels']))
print('b root nodes', [gb['nodes'][n]['name'] for n in gb['scenes'][0]['nodes']], [list(gb['nodes'][n].get('extras', {}).keys()) for n in gb['scenes'][0]['nodes']])
print('b materials extras', sum(1 for m in gb['materials'] if 'rasaMaterial' in m.get('extras', {})), len(gb['materials']))
print('b images', [i.get('uri') for i in gb.get('images', [])][:6])
