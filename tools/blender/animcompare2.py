"""Compare animation channel values (sampled at each original key time) between two glTF files, per bone and path."""
import json, struct, sys, os, math, bisect
import os
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "gltfcompare.py")).read().split("ga, ba = load")[0])
def channels(g, bins, anim_name):
    a = [x for x in g['animations'] if x['name'] == anim_name][0]
    out = {}
    for c in a['channels']:
        s = a['samplers'][c['sampler']]
        node = g['nodes'][c['target']['node']]['name']
        times = [t[0] for t in accessor(g, bins, s['input'])]
        vals = accessor(g, bins, s['output'])
        out[(node, c['target']['path'])] = (times, vals)
    return out
def sample(times, vals, t):
    i = bisect.bisect_right(times, t) - 1
    if i < 0: return vals[0]
    if i >= len(times) - 1: return vals[-1]
    f = (t - times[i]) / (times[i + 1] - times[i])
    a, b = vals[i], list(vals[i + 1])
    if len(a) == 4 and sum(x * y for x, y in zip(a, b)) < 0: b = [-x for x in b]
    return [x + (y - x) * f for x, y in zip(a, b)]
ga, ba = load(sys.argv[1]); gb, bb = load(sys.argv[2])
for name in sys.argv[3:]:
    ca, cb = channels(ga, ba, name), channels(gb, bb, name)
    worst = {}
    for key, (times, vals) in ca.items():
        if key not in cb: print('missing in b', key); continue
        tb, vb = cb[key]
        for t, v in zip(times, vals):
            w = sample(tb, vb, t)
            if key[1] == 'rotation':
                d = math.degrees(2 * math.acos(min(1, abs(sum(x * y for x, y in zip(v, w))))))
            else:
                d = max(abs(x - y) for x, y in zip(v, w))
            if d > worst.get(key[1], (0, None))[0]: worst[key[1]] = (d, key[0], t)
    print(name, 'largest differences at the original key times:', worst)
