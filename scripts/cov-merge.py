#!/usr/bin/env python3
# cov-merge.py OUTDIR [OUTDIR...] -> merged per-source coverage in cov.json (paths relative to the build
# tree $COV_TREE, default /root/cov/bt-cov)
# Sums the counts of all objects compiling the same source (PIC and non-PIC copies, several binaries).
import gzip, json, os, sys, glob, collections
ROOT = os.path.normpath(os.environ.get('COV_TREE', '/root/cov/bt-cov')) + '/'
lines = collections.defaultdict(lambda: collections.defaultdict(int))   # file -> line -> count
funcs = collections.defaultdict(dict)                                   # file -> name -> [count, start, end]
branches = collections.defaultdict(lambda: collections.defaultdict(int))  # file -> (line, idx) -> count
for d in sys.argv[1:]:
    for p in glob.glob(os.path.join(d, '*.gcov.json.gz')):
        j = json.load(gzip.open(p))
        cwd = j['current_working_directory']
        for f in j['files']:
            path = os.path.normpath(os.path.join(cwd, f['file']))
            if not path.startswith(ROOT):
                continue
            rel = path[len(ROOT):]
            for l in f['lines']:
                lines[rel][l['line_number']] += l['count']
                for i, b in enumerate(l.get('branches', [])):
                    branches[rel][(l['line_number'], i)] += b['count']
            for fn in f['functions']:
                name = fn.get('demangled_name', fn['name'])
                c = funcs[rel].get(name, [0, fn['start_line'], fn['end_line']])
                c[0] += fn['execution_count']
                funcs[rel][name] = c
out = {}
for rel in sorted(lines):
    L = lines[rel]; F = funcs[rel]; B = branches[rel]
    out[rel] = {
        'lines': len(L), 'lines_hit': sum(1 for c in L.values() if c),
        'functions': len(F), 'functions_hit': sum(1 for c in F.values() if c[0]),
        'branches': len(B), 'branches_hit': sum(1 for c in B.values() if c),
        'uncovered_functions': sorted((v[1], k) for k, v in F.items() if not v[0]),
        'line_counts': {str(k): v for k, v in sorted(L.items())},
    }
json.dump(out, open('cov.json', 'w'))
print(len(out), 'files')
