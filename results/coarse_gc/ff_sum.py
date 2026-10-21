#!/usr/bin/env python3
# ff_sum.py [GROUP] [tags...]: deferred-fold study: WAF (% vs base) and breakdown per submitted host page
import json, sys, glob, os, math
d = 'results/coarse_gc/'; g = sys.argv[1] if len(sys.argv) > 1 else 'ff'
WL = ['ycsba_d_t64','ycsbb_d_t64','ycsbfstd_d_t64','tpcc_d_t64','fio_randwrite_d','fileserverfs','ali3','spfresh','ali1','ssd','u55','u73','u37']
tags = sys.argv[2:] or sorted({os.path.basename(p)[:-5] for p in glob.glob(f'{d}{g}/*/*.json')})
def L(p):
    try: return json.load(open(p))
    except Exception: return None
def row(j):
    hs = j['host_pages']; p = j['prog']
    return 'WAF %7.3f finegc %5.2f fold %5.2f cgc %5.2f (clean %5.2f refold %5.2f) map %5.3f' % (j['WAF'], p['finegc']/hs, (p['fold']+p['zerofill'])/hs, (p['cgc_clean']+p['cgc_refold'])/hs, p['cgc_clean']/hs, p['cgc_refold']/hs, p['map']/hs)
for w in WL:
    b = L(f'{d}diag/{w}/base.json')
    if not b: continue
    print(f'{w:16s} {"base":12s}        {row(b)}')
    for t in tags:
        j = L(f'{d}{g}/{w}/{t}.json')
        if not j: continue
        ff = j.get('ffold', {})
        print(f'{w:16s} {t:12s} {100*(j["WAF"]/b["WAF"]-1):+6.1f}% {row(j)} | folds d/n/guard {ff.get("folds_d",0)}/{ff.get("folds_n",0)}/{ff.get("folds_guard",0)} forced {ff.get("forced_fold",0)} mapRd/rd {j.get("spill",{}).get("map_reads_per_host_read",0):.4f}')
# geomean table
print()
print('%-14s' % 'tag' + ' '.join('%7s' % w[:7] for w in WL) + '  gm_traces')
for t in tags:
    r = []; ok = []
    for w in WL:
        b = L(f'{d}diag/{w}/base.json'); j = L(f'{d}{g}/{w}/{t}.json')
        if b and j: r.append('%+6.1f%%' % (100*(j['WAF']/b['WAF']-1))); ok.append(j['WAF']/b['WAF']) if not w.startswith('u') else None
        else: r.append('%7s' % '-')
    gm = math.exp(sum(map(math.log, ok))/len(ok)) if ok else float('nan')
    print('%-14s' % t + ' '.join(r) + '  %+6.1f%%' % (100*(gm-1)))
