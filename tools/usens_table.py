#!/usr/bin/env python3
"""Coarse-IU sensitivity of DUET (U = 16 / 32 / 64 / 128 pages): results/final_vbm (U=16) and results/final_usens/u<U>
(q_usens.sh, 2026-11-09). Prints per-workload metrics normalized to Page, and writes results/paper_dac/usens_data.csv."""
import sys, os, csv, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import plot_eval as pe
US = [16, 32, 64, 128]
WL = [('raw', 'a128_r70_s70', '70:30'), ('trace', 'ali3', 'Alibaba'), ('trace', 'spfresh', 'SPFresh'),
      ('app', 'ycsba_d_t64', 'YCSB-A'), ('app', 'tpcc_d_t64', 'TPC-C')]
M = ['tput', 'fr', 'waf', 'r_avg', 'w_avg', 'r_p99', 'w_p99']

def duet(kind, key, u):
    if u == 16:
        return pe.row(kind, key, 'proposed')
    R = f'{pe.B}/final_usens/u{u}'
    if kind == 'app':
        d = f'{R}/apps/{key}/proposed_vbu{u}'
        s = dict(l.strip().split('=', 1) for l in open(f'{d}/summary.txt') if '=' in l)
        assert s['status'] == 'DONE', d
        g = lambda k: float(s[k])
        return {'tput': g('throughput'), 'fr': 1 + g('dev_map_reads_per_host_read'), 'waf': g('dev_waf'),
                'r_avg': g('blk_read_lat_us_mean') / 1e3, 'w_avg': g('blk_write_lat_us_mean') / 1e3,
                'r_p99': g('blk_read_lat_us_p99') / 1e3, 'w_p99': g('blk_write_lat_us_p99') / 1e3}
    return pe.replay(f'{R}/{"trace_" if kind == "trace" else "replay_"}{key}', 'proposed')

rows = []
for kind, key, lab in WL:
    pg = pe.row(kind, key, 'page4k')
    for u in US:
        d = duet(kind, key, u)
        rows.append([lab, u] + [round(d[m] if m == 'fr' else d[m] / pg[m], 3) for m in M])
gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
for u in US:
    rows.append(['GEOMEAN', u] + [round(gm([r[2 + i] for r in rows if r[1] == u and r[0] != 'GEOMEAN']), 3) for i in range(len(M))])
os.makedirs(pe.OUT, exist_ok=True)
with open(f'{pe.OUT}/usens_data.csv', 'w', newline='') as fo:
    w = csv.writer(fo); w.writerow(['workload', 'U'] + M); w.writerows(rows)
print('%-8s %4s ' % ('wl', 'U') + ' '.join('%6s' % m for m in M))
for r in rows: print('%-8s %4d ' % (r[0], r[1]) + ' '.join('%6.2f' % x for x in r[2:]))
