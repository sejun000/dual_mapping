#!/usr/bin/env python3
# diag_table.py GROUP TAG: coarse-GC diagnosis table from cgc_diag=1 runs
import json, sys
d = 'results/coarse_gc/'; g, t = (sys.argv[1:3] + ['diag', 'base'])[:2] if len(sys.argv) > 2 else ('diag', 'base')
WL = ['ycsba_d_t64','ycsbb_d_t64','ycsbfstd_d_t64','tpcc_d_t64','fio_randwrite_d','fileserverfs','ali3','spfresh','ali1','ssd']
print('%-15s %6s | %5s %5s %5s %5s | %5s %5s %5s | %5s %5s %5s | %6s %6s %5s | %5s %5s %5s' % ('workload','WAF','vict','min','cbV','chnk','old1L','hot','cold0','frag','refC','cpIU','fexHi','fexRf','fexFo','fineB','fragL','spare'))
for w in WL:
    try: j = json.load(open(f'{d}{g}/{w}/{t}.json'))
    except Exception: continue
    c = j['cgc_diag']; dg = j['diag']; P = j['P']; NB = j['NB']
    cp = c['copy_age']; tot = sum(cp)
    old = sum(cp[4:]) / tot if tot else 0
    hot = sum(cp[:2]) / tot if tot else 0
    cold0 = c['copies_by_hw'][0] / max(1, sum(c['copies_by_hw']))
    hs = j['host_pages']
    chunkv = dg['cgc_prog_per_victim_frac']
    fx = c['fine_exit_ow_hostiu_refold_fold']; fsum = max(1, sum(fx) + j['fine_host_ow'])
    nius = sum(c['ius_by_hw'])
    cpiu = tot / nius
    L = j['capacity_gb'] * 2**30 / 4096
    data_blocks = L / P
    print('%-15s %6.3f | %5.3f %5.3f %5.3f %5.3f | %5.3f %5.3f %5.3f | %5.3f %5.2f %5.1f | %6.3f %6.3f %5.3f | %5.1f %5.3f %5.1f' % (
        w, j['WAF'], c['victim_valid_frac'], c['min_valid_frac'], c['cb_valid_frac'], chunkv, old, hot, cold0,
        (j['prog']['cgc_refold'] / max(1, j['prog']['cgc_clean'] + j['prog']['cgc_refold'])), j['prog']['cgc_refold'] / hs, cpiu,
        fx[1] / fsum, fx[2] / fsum, fx[3] / fsum, dg['occ_fine_blocks'], dg['occ_fine_valid_frac_L'], NB - data_blocks))
print('''
vict/min/cbV: mean valid fraction of the chosen coarse victim / of the min-valid (greedy) block / of the cb choice, at each coarse GC
chnk: coarse GC programs per victim / P (live chunk pages incl. the fragments a refold rewrites)
old1L: share of GC-relocated chunks whose IU was not host-written for >= 1 device write (L host pages); hot: < L/8
cold0: share of relocations spent on IUs never host-written during the whole measurement
frag: share of coarse GC programs that are refolds; refC: refold programs per submitted host page; cpIU: GC relocations per IU in the measurement
fexHi/fexRf/fexFo: fine pages retired by host full-IU writes / refold / fold (rest: host 4K overwrite)
fineB: mean fine blocks; fragL: mean fine-resident pages / L (double occupancy); spare: NB - L/P blocks''')
