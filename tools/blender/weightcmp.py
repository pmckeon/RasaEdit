import json, sys, math
import os
exec(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "gltfcompare.py")).read().split("ga, ba = load")[0])
ga, ba = load(sys.argv[1]); gb, bb = load(sys.argv[2]); meshname = sys.argv[3]
def mesh(g, b, name):
    for m in g['meshes']:
        if m['name'] == name:
            p = m['primitives'][0]
            pos = accessor(g, b, p['attributes']['POSITION'])
            j = accessor(g, b, p['attributes']['JOINTS_0']); w = accessor(g, b, p['attributes']['WEIGHTS_0'])
            names = [g['nodes'][x]['name'] for x in g['skins'][0]['joints']]
            nrm = accessor(g, b, p['attributes']['NORMAL'])
            return [(pos[i], {names[j[i][k]]: w[i][k] for k in range(4) if w[i][k] > 0}, nrm[i]) for i in range(len(pos))]
A = mesh(ga, ba, meshname); B = mesh(gb, bb, meshname)
worst = 0; shown = 0
for p, wa, na in A:
    best = min(B, key=lambda q: sum((x - y) ** 2 for x, y in zip(p, q[0])) + 1e-7 * (1 - sum(x * y for x, y in zip(na, q[2]))))
    wb = best[1]
    d = max(abs(wa.get(k, 0) - wb.get(k, 0)) for k in set(wa) | set(wb))
    if d > worst: worst = d
    if d > 0.2 and shown < 4:
        shown += 1
        print('pos', [round(x, 3) for x in p], 'a', {k: round(v, 3) for k, v in wa.items()}, 'b', {k: round(v, 3) for k, v in wb.items()})
print('worst weight difference', round(worst, 4))
