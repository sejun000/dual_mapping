#!/usr/bin/env python3
"""Summarize results/spill_rp (spill read-promotion, ftlsim spill_rp / spill_rp_rate) -> results/spill_rp/SUMMARY.txt"""
import json, os, sys
R = 'results/spill_rp'
WL = ['spfresh', 'ycsba_d_t64', 'ycsbb_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'ali3', 'f73', 'f37']
TAGS = sys.argv[1:] or ['base', 'rp1', 'rp2', 'rp4', 'rp1r20', 'rp1r50', 'rp1r100', 'rp2r50', 'rpc1', 'rpc2', 'rpc4']


def ld(w, t):
    try: return json.load(open(f'{R}/{w}/{t}.json'))
    except Exception: return None


def row(w, t, j, b):
    s = j['spill']; h = j['host_pages']; rd = s['host_reads']
    ftpw = s['ftp_progs']['spill'] + s['ftp_progs']['gc_update']
    dw = (j['WAF'] / b['WAF'] - 1) * 100 if b else 0
    mrr = s['map_reads_per_host_read']; bm = b['spill']['map_reads_per_host_read'] if b else 0
    dm = (mrr / bm - 1) * 100 if bm else 0
    return (f"{w:15s} {t:10s} {j['WAF']:7.3f} {dw:+6.2f}% {j['prog'].get('map', 0) / h:8.4f} {ftpw / h:8.4f} {s.get('rp_ftp_progs', 0) / h:8.4f} "
            f"{mrr:8.4f} {dm:+7.1f}% {s['frac_reads_spilled']:7.4f} {s.get('rp_promotions', 0) / max(rd, 1) * 1000:8.3f} "
            f"{s.get('rp_ftp_reads', 0) / max(rd, 1) * 1000:8.3f} {s['spills'] / h * 1000:7.2f} {s['spilled_ius_end']:7d}")


out = ['spill read-promotion (ftlsim spill_rp=K [spill_rp_rate=R]) vs BASE = V3 + spill=2 + dram_stats=1 at map_budget 0.125',
       '(results/coarse_gc/base_params.txt + workloads.txt geometry; host reads replayed). ycsbc_d_t64 / ycsbe_d_t64 have no captured',
       'block trace (only NVMeVirt app runs exist), so ycsbfstd_d_t64 is the third YCSB trace. f73/f37 (synthetic, one read probe per',
       'write request) never spill at this budget: spill_rp must be (and is) a no-op there.',
       'Columns: WAF (programs / submitted host page) and change vs base; map = FTP programs per submitted host write page (prog map, in WAF);',
       'ftpW = same from the spill counters; rpW = FTP programs spent inside read-promotions per host write page; mr/rd = flash map reads',
       'per host read page (incl. the FTP read-modify-writes of promotions) and change vs base; spl = fraction of host reads that hit a',
       'spilled IU; prom/k, rpR/k = promotions and promotion FTP reads per 1000 host reads; spill/k = spill events per 1000 host write pages;',
       'sp_end = spilled IUs at the end.',
       f"{'workload':15s} {'variant':10s} {'WAF':>7s} {'dWAF':>7s} {'map/h':>8s} {'ftpW/h':>8s} {'rpW/h':>8s} {'mr/rd':>8s} {'dmr':>8s} "
       f"{'spl':>7s} {'prom/k':>8s} {'rpR/k':>8s} {'spill/k':>7s} {'sp_end':>7s}"]
for w in WL:
    b = ld(w, 'base')
    for t in TAGS:
        j = ld(w, t)
        if j: out.append(row(w, t, j, b))
    out.append('')
txt = '\n'.join(out) + '\n'
head = open(f'{R}/SUMMARY_head.txt').read() if os.path.exists(f'{R}/SUMMARY_head.txt') else ''
open(f'{R}/SUMMARY.txt', 'w').write(head + '\n' + txt)
print(txt)
