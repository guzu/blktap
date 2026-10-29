#!/usr/bin/env python3
"""lcov-filter.py [-f RULES]... [-e RULE]... IN.info OUT.info

Removes files, functions, lines or branches from an lcov tracefile, and recomputes its counters.
Rules (one per line in a RULES file, or one per -e; '#' starts a comment; separated by blanks):

  file     GLOB                     drop the files; GLOB is matched against the end of the path
                                    (qcow2/*, drivers/td-rated.c, *.h)
  function REGEX [GLOB]             drop the functions whose name matches REGEX, with their lines and
                                    branches; GLOB limits it to some files
  lines    GLOB START-END[,...]     drop the lines (and the branches, and the functions starting there)
                                    of a line range, e.g. drivers/tapdisk-utils.c 312-330,400-410
  omit     REGEX                    drop the lines whose source text matches REGEX (sources are read
                                    from the SF: paths of the tracefile), e.g. 'BUG_ON|ASSERT'

Source-level exclusion markers (LCOV_EXCL_LINE, LCOV_EXCL_START/STOP) are handled by lcov itself
when the trace is captured, not here.
"""
import fnmatch, re, sys

def parse_rules(lines):
    rules = []
    for n, raw in enumerate(lines, 1):
        s = raw.split('#', 1)[0].strip()
        if not s:
            continue
        kind, _, rest = s.partition(' ')
        arg = rest.split()
        if kind == 'file' and len(arg) == 1:
            rules.append(('file', arg[0]))
        elif kind == 'function' and len(arg) in (1, 2):
            rules.append(('function', re.compile(arg[0]), arg[1] if len(arg) == 2 else '*'))
        elif kind == 'lines' and len(arg) == 2:
            ranges = []
            for r in arg[1].split(','):
                a, _, b = r.partition('-')
                ranges.append((int(a), int(b or a)))
            rules.append(('lines', arg[0], ranges))
        elif kind == 'omit' and len(arg) >= 1:
            rules.append(('omit', re.compile(rest.strip())))
        else:
            sys.exit(f'bad rule: {raw.strip()!r}')
    return rules

def match(path, glob):
    return fnmatch.fnmatch(path, glob) or fnmatch.fnmatch(path, '*/' + glob)

def filter_record(rec, rules):
    sf = next(l[3:] for l in rec if l.startswith('SF:'))
    if any(r[0] == 'file' and match(sf, r[1]) for r in rules):
        return None
    drop = set()          # line numbers to drop (DA and BRDA)
    drop_fn = set()       # function indexes (FNL/FNA) or names (FN/FNDA) to drop
    fnl = {}
    for l in rec:
        if l.startswith('FNL:'):
            i, *rest = l[4:].split(',')
            fnl[i] = (int(rest[0]), int(rest[1]) if len(rest) > 1 else int(rest[0]))
    names = {}
    for l in rec:
        if l.startswith('FNA:'):
            i, _, name = l[4:].split(',', 2)
            names[i] = name
    for r in rules:
        if r[0] == 'function' and match(sf, r[2]):
            for i, name in names.items():
                if r[1].search(name):
                    drop_fn.add(i)
                    drop.update(range(fnl[i][0], fnl[i][1] + 1))
        elif r[0] == 'lines' and match(sf, r[1]):
            for a, b in r[2]:
                drop.update(range(a, b + 1))
            for i, (start, _) in fnl.items():
                if any(a <= start <= b for a, b in r[2]):
                    drop_fn.add(i)
        elif r[0] == 'omit':
            try:
                src = open(sf, errors='replace').read().split('\n')
            except OSError:
                src = []
            drop.update(n + 1 for n, t in enumerate(src) if r[1].search(t))
    out = []
    for l in rec:
        k, _, v = l.partition(':')
        if k in ('FNL', 'FNA') and v.split(',', 1)[0] in drop_fn:
            continue
        if k in ('DA', 'BRDA') and int(v.split(',', 1)[0]) in drop:
            continue
        out.append(l)
    fna = [l for l in out if l.startswith('FNA:')]
    da = [l for l in out if l.startswith('DA:')]
    br = [l for l in out if l.startswith('BRDA:')]
    if not (fna or da or br):
        return None
    cnt = lambda ls, f: sum(1 for l in ls if f(l.split(',')))
    for n, l in enumerate(out):
        if l.startswith('FNF:'): out[n] = f'FNF:{len(fna)}'
        elif l.startswith('FNH:'): out[n] = f'FNH:{cnt(fna, lambda p: int(p[1]) > 0)}'
        elif l.startswith('LF:'): out[n] = f'LF:{len(da)}'
        elif l.startswith('LH:'): out[n] = f'LH:{cnt(da, lambda p: int(p[1]) > 0)}'
        elif l.startswith('BRF:'): out[n] = f'BRF:{len(br)}'
        elif l.startswith('BRH:'): out[n] = f'BRH:{cnt(br, lambda p: p[3] not in ("-", "0"))}'
    return out

def main(argv):
    rules, files = [], []
    it = iter(argv)
    for a in it:
        if a == '-f':
            rules += parse_rules(open(next(it)).read().split('\n'))
        elif a == '-e':
            rules += parse_rules([next(it)])
        else:
            files.append(a)
    if len(files) != 2:
        sys.exit(__doc__)
    rec, out = [], []
    for l in open(files[0]).read().split('\n'):
        if not l:
            continue
        rec.append(l)
        if l == 'end_of_record':
            r = filter_record(rec, rules)
            if r:
                out += r
            rec = []
    open(files[1], 'w').write('\n'.join(out) + '\n')

main(sys.argv[1:])
