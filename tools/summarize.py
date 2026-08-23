#!/usr/bin/env python3
"""Summarize ftlsim JSON outputs in a directory into a WAF table + breakdown."""
import json, sys, glob, os
d = sys.argv[1] if len(sys.argv) > 1 else "results/kill"
rows = []
for f in sorted(glob.glob(os.path.join(d, "*.json"))):
    try: rows.append(json.load(open(f)))
    except Exception as e: print(f"skip {f}: {e}")
tags = sorted({r["tag"] for r in rows}, key=lambda t: (t.startswith("proposed")==False, t))
zs = sorted({r["zipf"] for r in rows})
print(f"{'WAF':<16}" + "".join(f"{'zipf='+str(z):>11}" for z in zs) + f"{'RAM(MiB) max':>14}")
for t in tags:
    line = f"{t:<16}"; mem = 0
    for z in zs:
        m = [r for r in rows if r["tag"] == t and r["zipf"] == z]
        line += f"{m[0]['WAF']:>11.3f}" if m else f"{'-':>11}"
        if m and 'mem' in m[0]: mem = max(mem, m[0]['mem']['total_mib'])
    print(line + f"{mem:>14.2f}")
print()
for r in rows:
    if not r["tag"].startswith("proposed"): continue
    pf = r["prog_frac"]; ev = r["events"]
    print(f"proposed zipf={r['zipf']}: WAF={r['WAF']:.3f} RAF={r['RAF']:.2f} f={r['f_end']:.3f} | prog frac: host={pf['host']:.2f} finegc={pf['finegc']:.2f} fold={pf['fold']:.2f} cgc_clean={pf['cgc_clean']:.2f} refold={pf['cgc_refold']:.2f} unfold={pf.get('unfold',0):.3f} zerofill={pf['zerofill']:.3f}")
    print(f"   events: finegc={ev['finegc']} blockfold={ev['blockfold']} fold={ev['fold']} refold={ev['refold']} unfold={ev.get('unfold',0)} cgc={ev['cgc']} extent_drop={ev['extent_drop']} | avg victim valid: finegc={r['avg_victim_valid']['finegc']:.0f} cgc={r['avg_victim_valid']['cgc']:.0f}")
    print(f"   d_hist fold  : {r['d_hist_fold']}")
    print(f"   d_hist refold: {r['d_hist_refold']}")
