#!/usr/bin/env python3
# fb_sum.py: interim table for fine_borrow / deferred fold on ycsba, fio_randwrite_d (aged traces) and f73/f37 (synthetic 96G raw fio)
import json, os
d = 'results/coarse_gc/'
WL = ['ycsba_d_t64', 'fio_randwrite_d', 'f73', 'f37']
TAGS = [('diag', 'base'), ('diag', 'page4k'), ('ff', 'd8'), ('ff', 'd12'), ('ff', 'n2'), ('ff', 'n3'), ('ff', 'g0.8'), ('ff', 'd12n3g0.9'),
        ('fb', 'fb4'), ('fb', 'fb8'), ('fb', 'fb16'), ('fb', 'fb24'), ('fb', 'fb8_d12'), ('fb', 'fb16_d12'), ('fb', 'fb16_n3'), ('fb', 'fb8_n2'),
        ('fb', 'fb16_g0.8'), ('fb', 'fb24_d12n3g0.9')]
def L(p):
    try: return json.load(open(p))
    except Exception: return None
def pct(h, q):
    t = sum(h); a = 0
    for i, x in enumerate(h):
        a += x
        if a >= q * t: return i
    return -1
for w in WL:
    b = L(f'{d}diag/{w}/base.json')
    print(f'\n{w}')
    print('  %-16s %7s %7s | %6s %6s %6s %6s %6s %6s | %5s %9s' % ('variant', 'WAF', 'vsBASE', 'host', 'finegc', 'fold', 'clean', 'refold', 'map', 'fineB', 'p10/50/90'))
    for g, t in TAGS:
        j = L(f'{d}{g}/{w}/{t}.json')
        if j is None and t == 'page4k': j = L(f'results/sim_budget/{w}/page4k.json')
        if not j: continue
        hs = j['host_pages']; p = j['prog']
        fh = j.get('fine_blocks_hist', {}).get('all')
        fb = '%d/%d/%d' % (pct(fh, .1), pct(fh, .5), pct(fh, .9)) if fh and sum(fh) else '-'
        occ = j.get('diag', {}).get('occ_fine_blocks', 0)
        print('  %-16s %7.3f %+6.1f%% | %6.2f %6.2f %6.2f %6.2f %6.2f %6.3f | %5.1f %9s' % (t, j['WAF'], 100 * (j['WAF'] / b['WAF'] - 1) if b else 0, p['host'] / hs,
              p.get('finegc', 0) / hs, (p.get('fold', 0) + p.get('zerofill', 0)) / hs, p.get('cgc_clean', 0) / hs, (p.get('cgc_refold', 0)) / hs, p.get('map', 0) / hs, occ, fb))
print('\nbreakdown = programs per submitted host page (page4k GC copies are under "clean"? no: page4k GC = pgc, not shown; WAF includes it). fineB = mean fine blocks at erases; p10/50/90 of the fine-block count.')
