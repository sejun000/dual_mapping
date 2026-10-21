#!/usr/bin/env python3
"""Part C2 table from results/sens_dram/<wl>/<tag>.json (ftlsim dram_stats=1)."""
import json, glob, os, sys
OUT = sys.argv[1] if len(sys.argv) > 1 else 'results/sens_dram'
POLS = [('prop_U16_b1875', 'proposed U16'), ('prop_U8_b1875', 'proposed U8'), ('prop_U32_b1875', 'proposed U32'),
        ('page4k', 'page4k'), ('dftl_b1875', 'DFTL'), ('leaftl_b1875', 'LeaFTL'), ('iurmw_U16', 'IU+RMW')]
lines = ['DRAM accesses (random cache-line touches of mapping structures) and flash map reads, per operation.',
         'rd = host read page (mean / max), fmr = flash map reads (translation page / LeaFTL frame load / misprediction);',
         'wr = host write page (foreground map work); gc = per relocated page; fold = per fold-programmed page;',
         'ev = cuckoo2 eviction folds per 1000 host write pages (acc per eviction mean/max); tot = all map accesses per host write page.', '']
hdr = f'{"workload":12s} {"policy":13s} {"rd mean":>8s} {"rd max":>6s} {"fmr/rd":>8s} {"fmr max":>7s} {"wr mean":>8s} {"wr max(pg/IU)":>13s} {"fmr/wr":>8s} {"gc":>6s} {"fmr/gc":>7s} {"fold":>6s} {"ev/1k":>7s} {"acc/ev":>12s} {"tot/wr":>7s} {"WAF":>7s}'
lines.append(hdr)
for wd in sorted(glob.glob(OUT + '/*/')):
    wl = os.path.basename(wd.rstrip('/'))
    for tag, lab in POLS:
        try: j = json.load(open(f'{wd}{tag}.json')); d = j['dram']
        except Exception: continue
        lines.append(f'{wl:12s} {lab:13s} {d["acc_per_host_read"]:8.3f} {d["max_acc_host_read"]:6d} {d["flash_map_reads_per_host_read"]:8.4f} {d["max_flash_map_reads_host_read"]:7d} '
                     f'{d["acc_per_host_write"]:8.3f} {str(d["max_acc_fine_page_write"]) + "/" + str(d["max_acc_iu_write"]):>13s} {d["flash_map_reads_per_host_write"]:8.4f} '
                     f'{d["acc_per_gc_page"]:6.3f} {d["flash_map_reads_per_gc_page"]:7.4f} {d["acc_per_fold_page"]:6.3f} {d["evict_folds_per_host_write"] * 1000:7.3f} '
                     f'{"%.0f/%d" % (d["acc_per_evict_fold"], d["max_acc_evict_fold"]):>12s} {d["acc_total_per_host_write"]:7.3f} {j["WAF"]:7.3f}')
    lines.append('')
s = '\n'.join(lines) + '\n'
open(OUT + '/SUMMARY.txt', 'w').write(s); print(s)
