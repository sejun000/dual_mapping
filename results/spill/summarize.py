#!/usr/bin/env python3
"""Summarize results/spill (spill + lazy fold) against the results/sim_budget baselines -> results/spill/SUMMARY.txt"""
import json, os, glob

R = 'results/spill'; B = 'results/sim_budget'
APPS = ['ycsba_d_t64', 'ycsbb_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'fio_randwrite_d', 'fileserverfs']
REAL = ['ali3', 'spfresh']


def ld(p):
    try: return json.load(open(p))
    except Exception: return None


def waf(j): return f"{j['WAF']:.3f}" if j else '-'


def br(j):
    if not j: return '-'
    h = j['host_pages']; p = j['prog']
    g = lambda *k: sum(p.get(x, 0) for x in k) / h
    return (f"host {g('host'):.2f} finegc {g('finegc'):.2f} fold {g('fold', 'zerofill'):.2f} cgc_clean {g('cgc_clean'):.2f} "
            f"cgc_refold {g('cgc_refold'):.2f} map {g('map'):.3f} rmw {g('rmw'):.3f} pgc {g('pgc'):.2f}")


out = []
out.append('WAF (programs / submitted host pages). prop = current proposed (V3 + onefront_fix=1 pool_cap=4, fold on eviction);')
out.append('s1 = spill=1 (spill the victim IU), s2 = spill=2 (spill every table-resident IU of the victim FTP), +r1 = fold_on_read=1,')
out.append('+r2/+r3/+r4 = fold_on_read=2 (only if >= 2 fragments) / 3 (on the 2nd read hit) / 4 (only if write-cold: no fragment in a host fine block younger than one pool turnover). prop/page4k/dftl/leaftl from results/sim_budget (same arguments).')
out.append('prop_1.0 = map_budget 1.0 (no evictions; the upper bound spill aims at).')
hdr = f"{'workload':16s} {'budget':>6s} " + ' '.join(f'{t:>8s}' for t in ['prop', 's1', 's2', 's1+r1', 's1+r2', 's1+r3', 's1+r4', 'prop_1.0', 'page4k', 'dftl', 'leaftl'])
out.append(hdr)
rows = []
for wl in APPS + REAL:
    for b in (['0.125', '0.1875'] if wl in APPS else ['0.125']):
        js = [ld(f'{B}/{wl}/prop_{b}.json'), ld(f'{R}/{wl}/s1_{b}.json'), ld(f'{R}/{wl}/s2_{b}.json'), ld(f'{R}/{wl}/s1r1_{b}.json'), ld(f'{R}/{wl}/s1r2_{b}.json'),
              ld(f'{R}/{wl}/s1r3_{b}.json'), ld(f'{R}/{wl}/s1r4_{b}.json'), ld(f'{B}/{wl}/prop_1.0.json'), ld(f'{B}/{wl}/page4k.json'), ld(f'{B}/{wl}/dftl_{b}.json'), ld(f'{B}/{wl}/leaftl_{b}.json')]
        out.append(f'{wl:16s} {b:>6s} ' + ' '.join(f'{waf(j):>8s}' for j in js))
out.append('')
out.append('WAF breakdown (programs per submitted host page)')
for wl in APPS + REAL:
    for b in (['0.125', '0.1875'] if wl in APPS else ['0.125']):
        for t, p in [('prop', f'{B}/{wl}/prop_{b}.json'), ('s1', f'{R}/{wl}/s1_{b}.json'), ('s2', f'{R}/{wl}/s2_{b}.json'), ('s1+r1', f'{R}/{wl}/s1r1_{b}.json'), ('s1+r3', f'{R}/{wl}/s1r3_{b}.json'), ('s1+r4', f'{R}/{wl}/s1r4_{b}.json')]:
            out.append(f'{wl:16s} {b:>6s} {t:6s} {br(ld(p))}')
out.append('')
out.append('Reads (dram_stats replay of the trace reads): host_reads = mapped-path host read pages (not served by the write buffer);')
out.append('spilled = fraction of host reads that hit a spilled IU; ftp = fraction that issued an FTP flash read (SRAM FTP buffer misses);')
out.append('frag = of the spilled hits, fraction whose page is a fragment (a per-IU bitmap would save the rest); mr/rd = flash map reads per host read.')
out.append('Events per 1000 submitted host pages: spills, un-spills; ftpR/ftpW = FTP reads / programs per host page; rfold = read-triggered folds.')
out.append(f"{'workload':16s} {'budget':>6s} {'var':6s} {'host_rd':>10s} {'spilled':>8s} {'ftp':>8s} {'frag':>6s} {'mr/rd':>8s} {'spill/k':>8s} {'unsp/k':>8s} {'ftpR/h':>7s} {'ftpW/h':>7s} {'rfold/k':>8s} {'rfprog/h':>8s} {'sp_end':>7s}")
for wl in APPS + REAL:
    for b in (['0.125', '0.1875'] if wl in APPS else ['0.125']):
        for t, p in [('s1', f'{R}/{wl}/s1_{b}.json'), ('s2', f'{R}/{wl}/s2_{b}.json'), ('s1+r1', f'{R}/{wl}/s1r1_{b}.json'), ('s1+r3', f'{R}/{wl}/s1r3_{b}.json'), ('s1+r2', f'{R}/{wl}/s1r2_{b}.json'), ('s1+r4', f'{R}/{wl}/s1r4_{b}.json')]:
            j = ld(p)
            if not j: continue
            s = j['spill']; h = j['host_pages']
            fr = sum(s['ftp_reads'].values()); fw = sum(s['ftp_progs'].values())
            out.append(f"{wl:16s} {b:>6s} {t:6s} {s['host_reads']:>10d} {s['frac_reads_spilled']:>8.4f} {s['frac_reads_ftp']:>8.4f} "
                       f"{(s['reads_spilled_frag'] / s['reads_spilled'] if s['reads_spilled'] else 0):>6.3f} {s['map_reads_per_host_read']:>8.4f} "
                       f"{1000 * s['spills'] / h:>8.2f} {1000 * s['unspills'] / h:>8.2f} {fr / h:>7.4f} {fw / h:>7.4f} {1000 * s['rfolds'] / h:>8.2f} "
                       f"{s['rfold_prog_per_host']:>8.4f} {s['spilled_ius_end']:>7d}")
        j = ld(f'{R}/{wl}/dftl_d_0.125.json')
        if j and b == '0.125':
            d = j['dram']
            out.append(f"{wl:16s} {b:>6s} {'dftl':6s} {d['host_read_pages']:>10d} {'':>8s} {'':>8s} {'':>6s} {d['flash_map_reads_per_host_read']:>8.4f}   (DFTL, dram_stats run: WAF {j['WAF']:.3f})")
out.append('')
out.append('DRAM split (bytes): coarse map / GTD / cuckoo table, budget')
seen = set()
for f in sorted(glob.glob(f'{R}/*/s1_*.json')):
    j = ld(f)
    if not j: continue
    s = j['spill']; k = (os.path.basename(os.path.dirname(f)), os.path.basename(f)[3:-5])
    out.append(f"{k[0]:16s} {k[1]:>6s} coarse {s['coarse_bytes']:.0f} + GTD {s['gtd_bytes']:.0f} ({s['ftps']} FTPs x 4 B) + table {s['table_bytes']:.0f} = "
               f"{s['coarse_bytes'] + s['gtd_bytes'] + s['table_bytes']:.0f} <= {s['budget_bytes']:.0f}; chunk index {s['coarse_entry_bits']['chunk']} bits + 2 flag bits")
open(f'{R}/SUMMARY.txt', 'w').write('\n'.join(out) + '\n')
print('\n'.join(out))
