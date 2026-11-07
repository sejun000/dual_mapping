#!/usr/bin/env python3
"""Summarize results/comap_sim (ftlsim mode=comap / dftl / fast at 1/8 mapping DRAM) against DUET BASE
(results/frag_bitmap/<w>/base.json) -> tables on stdout (SUMMARY.txt = SUMMARY_head.txt + these tables)."""
import json, os
R = 'results/comap_sim'
WL = ['spfresh', 'ycsba_d_t64', 'ycsbb_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'ali3', 'f73', 'f37']


def ld(p):
    try: return json.load(open(p))
    except Exception: return None


def mr(j):   # flash map reads per host read page
    return j.get('dram', {}).get('flash_map_reads_per_host_read', 0.0) if j else float('nan')


f = lambda x, w=7, p=3: f"{x:{w}.{p}f}" if x is not None and x == x else f"{'-':>{w}s}"
out = ['Table 1: WAF (programs incl. merges, GC moves and translation-page writes per submitted host page) and flash map reads per host read page',
       f"{'workload':15s} {'DUET':>7s} {'CoMap':>7s} {'CoM.9':>7s} {'DFTL':>7s} {'FAST':>7s} | {'DUET':>7s} {'CoMap':>7s} {'CoM.9':>7s} {'DFTL':>7s} {'FAST':>7s}   (map reads / host read)"]
for w in WL:
    d, c, c9, t, a = (ld(f'results/frag_bitmap/{w}/base.json'), ld(f'{R}/{w}/comap.json'), ld(f'{R}/{w}/comap_lf90.json'),
                      ld(f'{R}/{w}/dftl.json'), ld(f'{R}/{w}/fast.json'))
    W = lambda j: j['WAF'] if j else None
    M = lambda j: mr(j) if j else None
    out.append(f"{w:15s} {f(W(d))} {f(W(c))} {f(W(c9))} {f(W(t))} {f(W(a))} | {f(M(d), 7, 4)} {f(M(c), 7, 4)} {f(M(c9), 7, 4)} "
               f"{f(M(t), 7, 4)} {f(0.0 if a else None, 7, 4)}")
out += ['CoM.9 = CoMap with comap_logfrac=0.9 (10 log blocks, smaller CMT); FAST has no map reads (all maps in DRAM).']
out += ['', 'Table 2: CoMap internals (measured phase; per submitted host page unless noted)',
        f"{'workload':15s} {'host':>6s} {'merge':>7s} {'pgcmv':>6s} {'map':>6s} {'full':>7s} {'part':>5s} {'swit':>5s} {'pgc':>6s} "
        f"{'P_end':>6s} {'P_mean':>6s} {'pm_end':>6s} {'wr_pg%':>6s} {'rdP%':>6s} {'types r/t/s':>14s} {'mem_MiB':>7s}"]
for w, tag in [(w, t) for t in ('comap', 'comap_lf90') for w in WL]:
    c = ld(f'{R}/{w}/{tag}.json')
    if not c: out.append(f"{w:15s} (missing)"); continue
    w = w if tag == 'comap' else w + ' .9' 
    h = c['host_pages']; p = c['prog']; e = c['events']; m = c['comap']
    wr = m['write_req_page'] + m['write_req_block']; rd = m['reads_block'] + m['reads_page']
    out.append(f"{w:15s} {p['host'] / h:6.3f} {(p['fullmerge'] + p['partialmerge']) / h:7.3f} {p['pgc'] / h:6.3f} {p['map'] / h:6.4f} "
               f"{e['full']:7d} {e['partial']:5d} {e['switch']:5d} {e['page_gc']:6d} {m['P_end']:6.3f} {m['P_mean']:6.3f} "
               f"{m['page_mapped_frac_end']:6.4f} {100 * m['write_req_page'] / max(wr, 1):6.1f} {100 * m['reads_page'] / max(rd, 1):6.1f} "
               f"{'/'.join(str(x) for x in m['access_type']):>14s} {c['mem']['total_bytes'] / 2**20:7.2f}")
out += ['', 'Columns (Table 2): host / merge / pgcmv / map = host, FAST merge (full + partial), page-area GC move and translation-page',
        'programs per host page; full / part / swit = FAST merges; pgc = page-area GC runs; P_end / P_mean = page-mapping probability',
        'at the end / mean over measured periods; pm_end = page-mapped share of the logical pages at the end; wr_pg% = write requests',
        'routed to page mapping; rdP% = host read pages served by page-mapped data; types = measured periods classified',
        'random / temporal / spatial; mem = mapping DRAM at the end (budget: B12).']
print('\n'.join(out))
