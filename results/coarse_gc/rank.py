#!/usr/bin/env python3
# rank.py [GROUPS] : every tag found in the groups (comma list, default all), % WAF change vs base per workload,
# geomean over apps (6) / real traces (4) / all; sorted by all-geomean. Only tags complete on all 10 workloads.
import sys, json, os, glob, math
d = 'results/coarse_gc/'
groups = (sys.argv[1] if len(sys.argv) > 1 else 'pol,fq,age,vict,diag,ora').split(',')
WL = ['ycsba_d_t64','ycsbb_d_t64','ycsbfstd_d_t64','tpcc_d_t64','fio_randwrite_d','fileserverfs','ali3','spfresh','ali1','ssd']
SH = ['ya','yb','yf','tpcc','fio','fsrv','ali3','spf','ali1','ssd']
base = {w: json.load(open(f'{d}diag/{w}/base.json'))['WAF'] for w in WL}
res = {}
for g in groups:
    for p in glob.glob(f'{d}{g}/*/*.json'):
        w = p.split('/')[-2]; t = g + ':' + os.path.basename(p)[:-5]
        try: res.setdefault(t, {})[w] = json.load(open(p))['WAF']
        except Exception: pass
def gm(xs): return math.exp(sum(math.log(x) for x in xs) / len(xs))
rows = []
for t, r in res.items():
    if len(r) < len(WL): continue
    rel = [r[w] / base[w] for w in WL]
    rows.append((gm(rel), gm(rel[:6]), gm(rel[6:]), t, rel))
rows.sort()
print('%-34s %7s %7s %7s  ' % ('tag', 'all', 'apps', 'real') + ' '.join('%6s' % s for s in SH))
for a, b, c, t, rel in rows:
    print('%-34s %+6.1f%% %+6.1f%% %+6.1f%%  ' % (t[:34], 100*(a-1), 100*(b-1), 100*(c-1)) + ' '.join('%+6.1f' % (100*(x-1)) for x in rel))
