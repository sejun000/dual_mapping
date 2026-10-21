#!/usr/bin/env python3
# sum.py GROUP [-b] [-r REFGROUP:REFTAG] [tags...]: WAF table (workload x tag); -b adds the per-source breakdown
import sys, json, os, glob
d = 'results/coarse_gc/'
args = sys.argv[1:]; brk = '-b' in args; args = [a for a in args if a != '-b']
ref = None
if args and args[0] == '-r': ref = args[1]; args = args[2:]
g = args[0]; tags = args[1:]
WL = ['ycsba_d_t64','ycsbb_d_t64','ycsbfstd_d_t64','tpcc_d_t64','fio_randwrite_d','fileserverfs','ali3','spfresh','ali1','ssd']
def load(p):
    try: return json.load(open(p))
    except Exception: return None
groups = g.split(',')
if not tags:
    s = set()
    for gg in groups:
        for p in glob.glob(d + gg + '/*/*.json'): s.add(os.path.basename(p)[:-5])
    tags = sorted(s)
def find(w, t):
    for gg in groups:
        j = load(f'{d}{gg}/{w}/{t}.json')
        if j: return j
    return None
refv = {}
if ref:
    rg, rt = ref.split(':')
    for w in WL:
        j = load(f'{d}{rg}/{w}/{rt}.json'); refv[w] = j['WAF'] if j else None
print('%-16s' % 'workload' + ''.join('%11s' % t[:10] for t in tags))
for w in WL:
    row = []
    for t in tags:
        j = find(w, t)
        if not j: row.append('%11s' % '-'); continue
        if ref and refv.get(w): row.append('%+10.1f%%' % (100 * (j['WAF'] / refv[w] - 1)))
        else: row.append('%11.4f' % j['WAF'])
    if any(x.strip() != '-' for x in row): print('%-16s' % w + ''.join(row))
if brk:
    print('\nbreakdown per submitted host page')
    for w in WL:
        for t in tags:
            j = find(w, t)
            if not j: continue
            hs = j['host_pages']; p = j['prog']
            sp = j.get('spill', {})
            print('%-16s %-12s WAF %.3f host %.2f finegc %.2f fold %.2f clean %.2f refold %.2f map %.3f zf %.3f rmw %.3f | cgcV %.3f fineV %.3f fq %d mapRd/rd %.4f' % (
                w, t[:12], j['WAF'], p['host']/hs, p['finegc']/hs, p['fold']/hs, p['cgc_clean']/hs, p['cgc_refold']/hs, p['map']/hs, p['zerofill']/hs, p['rmw']/hs,
                j['diag']['cgc_victim_valid_frac'], j['diag']['fine_victim_valid_frac'], j['fine_quota_blocks'], sp.get('map_reads_per_host_read', 0)))
