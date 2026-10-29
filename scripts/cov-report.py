#!/usr/bin/env python3
# cov-report.py cov.json: coverage per component and per file, from the output of cov-merge.py
import json, collections, sys
d = json.load(open(sys.argv[1]))
def comp(f):
    if f.startswith('drivers/block-'): return 'drivers/block-*'
    if f.startswith('drivers/'): return 'drivers (core)'
    if f.startswith('qcow2/lib/'): return 'qcow2/lib (QEMU)'
    if f.startswith('qcow2/'): return 'qcow2 (glue)'
    return f.split('/')[0]
agg = collections.defaultdict(lambda: [0]*6)
for f, v in d.items():
    a = agg[comp(f)]
    for i, k in enumerate(('lines','lines_hit','functions','functions_hit','branches','branches_hit')): a[i] += v[k]
pc = lambda h, t: f'{100*h/t:5.1f}%' if t else '   - '
print(f"{'component':20} {'lines':>13} {'func':>11} {'branch':>13}")
tot=[0]*6
for c, a in sorted(agg.items(), key=lambda x: -x[1][0]):
    tot=[x+y for x,y in zip(tot,a)]
    print(f"{c:20} {a[1]:5}/{a[0]:5} {pc(a[1],a[0])} {a[3]:4}/{a[2]:4} {pc(a[3],a[2])} {pc(a[5],a[4])}")
print(f"{'TOTAL':20} {tot[1]:5}/{tot[0]:5} {pc(tot[1],tot[0])} {tot[3]:4}/{tot[2]:4} {pc(tot[3],tot[2])} {pc(tot[5],tot[4])}")
print()
for f, v in sorted(d.items()):
    if comp(f) in ('drivers (core)','drivers/block-*','tapback','control','vhd') and v['lines']:
        print(f"{f:40} {v['lines_hit']:5}/{v['lines']:5} {pc(v['lines_hit'],v['lines'])}  fn {v['functions_hit']}/{v['functions']}  br {pc(v['branches_hit'],v['branches'])}")
