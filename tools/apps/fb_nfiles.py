#!/usr/bin/env python3
"""fb_nfiles.py <file.f> <target_pct> <capacity_bytes> <used_bytes>
Prints the $nfiles that makes the filebench filesets of <file.f> fill the file system to <target_pct> of
<capacity_bytes> (df used+avail), given <used_bytes> already in use. Model: every preallocated file occupies its
size rounded up to 4 KiB (filebench 1.5 sizes are constant: the study log shows bigfileset total = nfiles x size);
prealloc=P creates P% of the entries. Filesets not sized by $nfiles (hot sets, OLTP log) are kept as they are."""
import math, re, sys

def num(v, vars_):
    v = v.strip()
    if v.startswith('$'):
        v = vars_[v[1:]]
    m = re.fullmatch(r'([\d.]+)([kmgKMG]?)', str(v))
    mult = {'': 1, 'k': 1 << 10, 'm': 1 << 20, 'g': 1 << 30}[m.group(2).lower()]
    return float(m.group(1)) * mult

path, target, cap, used = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
vars_, sets = {}, []
for line in open(path):
    m = re.match(r'\s*set \$(\w+)=(\S+)', line)
    if m:
        vars_[m.group(1)] = m.group(2)
    m = re.match(r'\s*define fileset (.*)', line)
    if m:
        attrs = dict(kv.split('=', 1) if '=' in kv else (kv, '') for kv in m.group(1).split(','))
        sets.append(attrs)
blk = lambda b: math.ceil(b / 4096) * 4096
fixed, per_file = 0.0, None
for a in sets:
    size = blk(num(a['size'], vars_)); pre = float(a.get('prealloc', '0') or 0) / 100
    if a['entries'].strip() == '$nfiles':
        per_file = size * pre
    else:
        fixed += size * pre * num(a['entries'], vars_)
need = target / 100 * cap - used - fixed
n = max(1, int(need // per_file))
print(n, int(fixed), int(per_file))
