#!/usr/bin/env python3
"""Fig. 10: coarse-IU size sensitivity of DUET (U = 16 / 32 / 64 / 128 pages = 64 / 128 / 256 / 512 KB coarse IU), same
mapping DRAM budget. Data: results/paper_dac/usens_data.csv (tools/usens_table.py). (a) throughput, (b) WA, both / Page.
-> results/paper_dac/fig_usens.{pdf,png} and paper/figs (2026-11-09)."""
import csv, shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch
OUT = '/home/sejun000/dual_mapping/results/paper_dac'; PAPER = '/home/sejun000/dual_mapping/paper/figs'
plt.rcParams.update({'font.size': 7, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4, 'axes.linewidth': 0.6})
US = [16, 32, 64, 128]
COL = {16: '#3F2FD0', 32: '#8A80E6', 64: '#C3BDF2', 128: '#ECEAFB'}
LAB = {u: f'{u * 4} KB' + (' coarse IU (default)' if u == 16 else '') for u in US}
rows = list(csv.DictReader(open(f'{OUT}/usens_data.csv')))
WLS = [w for w in dict.fromkeys(r['workload'] for r in rows) if w != 'GEOMEAN']   # no geomean bar (2026-11-10)
WLN = {w: ('Geo. mean' if w == 'GEOMEAN' else w) for w in WLS}
val = {(r['workload'], int(r['U'])): r for r in rows}

fig, axs = plt.subplots(1, 2, figsize=(3.4, 0.9))   # was 1.15 (2026-11-10)
for ax, m, yl, ttl in ((axs[0], 'tput', 'Throughput', '(a) Throughput'), (axs[1], 'waf', 'WA', '(b) Write amplification')):
    bw = 0.8 / len(US)
    for i, u in enumerate(US):
        xs = [k + (i - 1.5) * bw for k in range(len(WLS))]
        ax.bar(xs, [float(val[(w, u)][m]) for w in WLS], bw, color=COL[u], edgecolor='black', linewidth=0.35)
    ax.axhline(1.0, color='#444441', lw=0.6, ls='--', zorder=0)
    ax.set_xticks(range(len(WLS))); ax.set_xticklabels([WLN[w] for w in WLS], fontsize=5.2, rotation=30, ha='right',
                                                        rotation_mode='anchor')
    ax.set_xlim(-0.55, len(WLS) - 0.45)
    ax.set_ylabel(yl + ' / Page', fontsize=6.0, labelpad=1.5); ax.set_xlabel(ttl, fontsize=6.4, labelpad=1.5)
    ax.tick_params(axis='y', labelsize=5.4, pad=1); ax.tick_params(axis='x', pad=1, length=0)
    ax.grid(axis='y', alpha=0.3, lw=0.5); ax.set_axisbelow(True)
axs[0].set_ylim(0, 1.4); axs[0].set_yticks([0, 0.5, 1]); axs[1].set_ylim(0, 2.1); axs[1].set_yticks([0, 1, 2])
fig.tight_layout(pad=0.2, w_pad=0.8); fig.subplots_adjust(top=0.80)   # was 0.84 (2026-11-10)
h = [Patch(fc=COL[u], ec='black', lw=0.35, label=LAB[u]) for u in US]
fig.legend(handles=h, ncol=4, fontsize=5.8, loc='upper center', bbox_to_anchor=(0.5, 1.02), frameon=False,
           handlelength=1.2, columnspacing=1.0, handletextpad=0.4)
for ext in ('pdf', 'png'):
    fig.savefig(f'{OUT}/fig_usens.{ext}', dpi=250, bbox_inches='tight', pad_inches=0.02)
shutil.copy(f'{OUT}/fig_usens.pdf', f'{PAPER}/fig_usens.pdf')
print(f'wrote {OUT}/fig_usens.{{pdf,png}} and paper/figs')
