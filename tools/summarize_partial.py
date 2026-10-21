#!/usr/bin/env python3
"""Summarize tools/run_partial_sweep.sh output: WAF table (fresh fill fractions and aged device x skew) and, for the
proposed configs, where the programs come from (per submitted host page) plus pool diagnostics.
  tools/summarize_partial.py results/partial/sweep > results/partial/sweep/SUMMARY.txt"""
import glob, json, os, re, sys

root = sys.argv[1] if len(sys.argv) > 1 else 'results/partial/sweep'
POLS = ['V3', 'V3F', 'page4k', 'dftl', 'leaftl']


def load(p):
    try:
        return json.load(open(p))
    except Exception:
        return None


def key(d):
    m = re.match(r'(fresh|aged)_f([0-9.]+)_z([0-9.]+)', os.path.basename(d))
    return (float(m.group(3)), m.group(1) == 'aged', float(m.group(2))) if m else (9, 9, 9)


dirs = sorted(glob.glob(root + '/*_f*_z*'), key=key)
print('WAF (NAND programs / submitted host pages), NVMeVirt 44G geometry, OP 7%, 4-16 KiB requests;')
print('fresh = seq fill of fill_frac then workload over fill_frac-0.03; aged = full seq fill, workload over 0.73')
print(f"{'condition':22s}" + ''.join(f'{p:>9s}' for p in POLS) + '   V3/p4k  V3F/p4k  V3F/V3')
for d in dirs:
    r = {p: load(f'{d}/{p}.json') for p in POLS}
    w = {p: (r[p]['WAF'] if r[p] else float('nan')) for p in POLS}
    print(f'{os.path.basename(d):22s}' + ''.join(f'{w[p]:9.3f}' for p in POLS) +
          f"   {w['V3'] / w['page4k']:6.2f}  {w['V3F'] / w['page4k']:7.2f}  {w['V3F'] / w['V3'] - 1:+7.1%}")
print()
print('Program sources per submitted host page and diagnostics (proposed):')
print('  finegc = fine-pool GC copies, fold = explicit folds (cuckoo eviction + fine reclaim), cgc = coarse GC (clean + refold);')
print('  fvv/cvv = mean valid fraction of fine / coarse GC victims, fblk = mean fine blocks, fq = mean fine cap,')
print('  util = fine pool valid / fine pool slots, evict = cuckoo eviction folds, sw = adapt=2 mode switches, uni = host share in uniform mode')
for d in dirs:
    for p in ('V3', 'V3F'):
        j = load(f'{d}/{p}.json')
        if not j:
            continue
        h = j['host_pages']; g = j['prog']; x = j.get('diag', {})
        print(f"{os.path.basename(d):22s} {p:4s} WAF={j['WAF']:6.3f} host={g['host'] / h:5.3f} finegc={g['finegc'] / h:5.3f} "
              f"fold={(g['fold'] + g['zerofill']) / h:5.3f} cgc={(g['cgc_clean'] + g['cgc_refold']) / h:5.3f} rmw={g['rmw'] / h:5.3f} | "
              f"fvv={x.get('fine_victim_valid_frac', 0):.2f} cvv={x.get('cgc_victim_valid_frac', 0):.2f} "
              f"fblk={x.get('occ_fine_blocks', 0):5.1f} fq={x.get('occ_quota', 0):5.1f} util={x.get('occ_fine_util', 0):.2f} "
              f"evict={x.get('evict_folds', 0)} sw={x.get('mode_switches', 0)} uni={x.get('uni_host_frac', 0):.2f}")
    for p in ('page4k',):
        j = load(f'{d}/{p}.json')
        if j:
            print(f"{os.path.basename(d):22s} {p:6s} WAF={j['WAF']:6.3f} gc={j['prog']['pgc'] / j['host_pages']:5.3f} "
                  f"victim_valid={j['avg_victim_valid']['pgc'] / j['P']:.2f}")
