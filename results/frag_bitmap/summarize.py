#!/usr/bin/env python3
"""Summarize results/frag_bitmap (ftlsim pbm=1 / ssum=1 vs BASE) -> results/frag_bitmap/SUMMARY.txt (SUMMARY_head.txt + tables)"""
import json, os, sys
R = 'results/frag_bitmap'
WL = ['spfresh', 'ycsba_d_t64', 'ycsbb_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'ali3', 'f73', 'f37']
TAGS = sys.argv[1:] or ['base', 'pbm', 'ssum', 'ssum2', 'bp', 'pbmp', 'ns', 'nspbm']


def ld(w, t):
    try: return json.load(open(f'{R}/{w}/{t}.json'))
    except Exception: return None


def waf_row(w, t, j, b):
    h = j['host_pages']; p = j['prog']
    dw = (j['WAF'] / b['WAF'] - 1) * 100 if b else 0
    s = j.get('spill', {})
    g = lambda *k: sum(p.get(x, 0) for x in k) / h
    return (f"{w:15s} {t:6s} {j['WAF']:7.3f} {dw:+6.2f}% {g('host'):6.3f} {g('finegc'):6.3f} {g('fold', 'zerofill'):6.3f} "
            f"{g('cgc_clean', 'cgc_refold'):6.3f} {g('rmw'):6.3f} {g('map'):7.4f} {s.get('ftp_progs_per_host_write', 0):7.4f} "
            f"{s.get('spills', 0) / h * 1000:7.2f} {j['events']['fold']:9d} {s.get('spilled_ius_end', 0):7d}")


def rd_row(w, t, j, b):
    f = j['fragmap']; s = j.get('spill', {}); rd = max(f['host_reads'], 1)
    mr = s.get('map_reads_per_host_read', 0); bm = b.get('spill', {}).get('map_reads_per_host_read', 0) if b else 0
    dm = (mr / bm - 1) * 100 if bm else 0
    occ = f['page_entries_end'] / f['table_entries']
    return (f"{w:15s} {t:6s} {mr:7.4f} {dm:+7.1f}% {f['chunk_ftp_per_read']:7.4f} {f['frag_ftp_per_read']:7.4f} {f['other_map_per_read']:7.4f} "
            f"{f['reads_spilled'] / rd:6.3f} {f['ftp_avoided'] / rd * 1000:7.1f} {f['ftp_avoided_flash'] / rd * 1000:7.1f} "
            f"{s.get('rp_promotions', 0) / rd * 1000:7.1f} {f['table_entries']:8d} {occ:6.3f} {f['summaries_end']:6d} {f['sum_hits'] / max(f['sum_hits'] + f['sum_misses'], 1):6.3f} "
            f"{j['dram']['acc_per_host_read']:6.2f} {j['dram']['acc_per_host_write']:6.2f}")


H1 = (f"{'workload':15s} {'var':6s} {'WAF':>7s} {'dWAF':>7s} {'host':>6s} {'fgc':>6s} {'fold':>6s} {'cgc':>6s} {'rmw':>6s} {'map':>7s} "
      f"{'ftpW/h':>7s} {'spill/k':>7s} {'folds':>9s} {'sp_end':>7s}")
H2 = (f"{'workload':15s} {'var':6s} {'mr/rd':>7s} {'dmr':>8s} {'chFTP':>7s} {'frFTP':>7s} {'other':>7s} "
      f"{'spl':>6s} {'avd/k':>7s} {'avdF/k':>7s} {'prom/k':>7s} {'tbl_cap':>8s} {'occ':>6s} {'sums':>6s} {'s_hit':>6s} {'dA/rd':>6s} {'dA/wr':>6s}")
def compact(w, t, j, b):
    f = j['fragmap']; s = j.get('spill', {}); h = j['host_pages']; rd = max(f['host_reads'], 1)
    dw = (j['WAF'] / b['WAF'] - 1) * 100 if b else 0
    mr = s.get('map_reads_per_host_read', 0); bm = b.get('spill', {}).get('map_reads_per_host_read', 0) if b else 0
    dm = f"{(mr / bm - 1) * 100:+6.1f}%" if bm else '      -'
    return (f"{w:15s} {t:6s} {j['WAF']:7.3f} {dw:+6.2f}% {j['prog'].get('map', 0) / h:7.4f} {mr:7.4f} {dm} {f['chunk_ftp_per_read']:7.4f} "
            f"{f['frag_ftp_per_read']:7.4f} {f['ftp_avoided'] / rd * 1000:6.1f} {s.get('spilled_ius_end', 0):7d} {f['table_entries']:8d} "
            f"{f['page_entries_end'] / f['table_entries']:5.3f} {f['summaries_end']:6d}")


H0 = (f"{'workload':15s} {'var':6s} {'WAF':>7s} {'dWAF':>7s} {'map/h':>7s} {'mr/rd':>7s} {'dmr':>7s} {'chFTP':>7s} {'frFTP':>7s} {'avd/k':>6s} "
      f"{'sp_end':>7s} {'tbl_cap':>8s} {'occ':>5s} {'sums':>6s}")
out = ['Table 0 (compact): BASE vs pbm=1 vs ssum=1 at the same 12.5 % mapping-DRAM budget', H0]
for w in WL:
    b = ld(w, 'base')
    for t in ['base', 'pbm', 'ssum']:
        j = ld(w, t)
        if j: out.append(compact(w, t, j, b))
out.append('')
out += ['Table A: WAF and its breakdown (programs per submitted host page; WAF counts FTP programs as prog map)', H1]
for w in WL:
    b = ld(w, 'base')
    for t in TAGS:
        j = ld(w, t)
        if j: out.append(waf_row(w, t, j, b if not t.startswith('ns') else ld(w, 'ns')))
    out.append('')
out += ['Table B: host-read map traffic and table state (per host read page unless noted)', H2]
for w in WL:
    b = ld(w, 'base')
    for t in TAGS:
        j = ld(w, t)
        if j and 'fragmap' in j: out.append(rd_row(w, t, j, b))
    out.append('')
txt = '\n'.join(out) + '\n'
head = open(f'{R}/SUMMARY_head.txt').read() if os.path.exists(f'{R}/SUMMARY_head.txt') else ''
open(f'{R}/SUMMARY.txt', 'w').write(head + '\n' + txt)
print(txt)
