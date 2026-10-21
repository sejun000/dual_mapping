#!/usr/bin/env python3
"""DAC paper figures from the final NVMeVirt matrix (results/final_b12/figs/data.csv, written by tools/plot_final.py).

fig_throughput : throughput normalized to page4k, 7 policies, groups: real traces | applications | raw fio (2x2)
fig_readlat    : read latency, average (bars) and p99 (markers), log scale, page4k / proposed / DFTL / LeaFTL
fig_waf        : device WAF for selected workloads (half column)
Outputs: results/final_b12/paper/<name>.{pdf,png}
"""
import csv, math, os
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

F = '/home/sejun000/dual_mapping/results/final_b12'
OUT = F + '/paper'
POL = ['page4k', 'proposed', 'dftl', 'leaftl', 'fast', 'mu', 'iurmw']
LABEL = {'page4k': 'Page (100% DRAM)', 'proposed': 'Proposed', 'dftl': 'DFTL', 'leaftl': 'LeaFTL', 'fast': 'FAST',
         'mu': 'μ-FTL', 'iurmw': 'IU+RMW'}
COLOR = {'page4k': '#B4B2A9', 'proposed': '#534AB7', 'dftl': '#1D9E75', 'leaftl': '#D85A30', 'fast': '#BA7517',
         'mu': '#D4537E', 'iurmw': '#378ADD'}
HATCH = {'page4k': '', 'proposed': '', 'dftl': '//', 'leaftl': '\\\\', 'fast': '..', 'mu': 'xx', 'iurmw': '--'}
# (group, csv workload name, short label)
GROUPS = [
    ('Real traces', [('Alibaba 1', 'Ali-high'), ('Alibaba 3', 'Ali-low'), ('SSD', 'SSD'), ('SPFresh', 'SPFresh')]),
    ('Applications', [('YCSB-A', 'A'), ('YCSB-B', 'B'), ('YCSB-C', 'C'), ('YCSB-D', 'D'), ('YCSB-E', 'E'),
                      ('YCSB-F', 'F'), ('TPC-C', 'TPC-C')]),
    ('Raw fio (uniform random)', [('fio R50:W50 4K:128K=70:30', 'R50\n4K-hv'), ('fio R50:W50 4K:128K=30:70', 'R50\n128K-hv'),
                                  ('fio R90:W10 4K:128K=70:30', 'R90\n4K-hv'), ('fio R90:W10 4K:128K=30:70', 'R90\n128K-hv')]),
]
plt.rcParams.update({'font.size': 8, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4, 'axes.linewidth': 0.6})


def load():
    d = {}
    for r in csv.DictReader(open(F + '/figs/data.csv')):
        d[(r['workload'], r['policy'])] = {k: float(v) if v not in ('', 'nan') else math.nan for k, v in r.items()
                                           if k not in ('group', 'workload', 'policy')}
    return d


def layout():
    xs, labels, seps, gcent = [], [], [], []
    x = 0
    for gi, (g, wls) in enumerate(GROUPS):
        start = x
        for _, lab in wls:
            xs.append(x); labels.append(lab); x += 1
        gcent.append(((start + x - 1) / 2, g))
        if gi < len(GROUPS) - 1:
            seps.append(x - 0.5 + 0.35); x += 0.7
    return xs, labels, seps, gcent


def finish(ax, xs, labels, seps, gcent, ylabel):
    ax.set_xticks(xs); ax.set_xticklabels(labels, fontsize=7)
    for s in seps:
        ax.axvline(s, color='#888780', lw=0.6, ls=':')
    for c, g in gcent:
        ax.text(c, 1.02, g, transform=ax.get_xaxis_transform(), ha='center', va='bottom', fontsize=8, fontweight='bold')
    ax.set_ylabel(ylabel, fontsize=8)
    ax.grid(axis='y', alpha=0.3, lw=0.5); ax.set_axisbelow(True)
    ax.set_xlim(xs[0] - 0.6, xs[-1] + 0.6)


def wl_list():
    return [w for _, wls in GROUPS for w, _ in wls]


def fig_throughput(d):
    xs, labels, seps, gcent = layout()
    fig, ax = plt.subplots(figsize=(7.0, 2.2))
    pols = POL; nb = len(pols); bw = 0.84 / nb
    for i, p in enumerate(pols):
        ys = [d[(w, p)]['throughput'] / d[(w, 'page4k')]['throughput'] for w in wl_list()]
        ax.bar([x + (i - (nb - 1) / 2) * bw for x in xs], ys, bw, color=COLOR[p], hatch=HATCH[p], edgecolor='white',
               linewidth=0.3, label=LABEL[p])
    ax.axhline(1.0, color='#444441', lw=0.6, ls='--')
    ax.set_ylim(0, 1.3)
    finish(ax, xs, labels, seps, gcent, 'Throughput\n(norm. to Page)')
    ax.legend(ncol=7, fontsize=6.5, loc='upper center', bbox_to_anchor=(0.5, 1.33), frameon=False, handlelength=1.4,
              columnspacing=0.9)
    fig.tight_layout(pad=0.3)
    save(fig, 'fig_throughput')


def fig_readlat(d):
    xs, labels, seps, gcent = layout()
    fig, ax = plt.subplots(figsize=(7.0, 2.1))
    pols = ['page4k', 'proposed', 'dftl', 'leaftl']; nb = len(pols); bw = 0.8 / nb
    for i, p in enumerate(pols):
        px = [x + (i - (nb - 1) / 2) * bw for x in xs]
        avg = [d[(w, p)]['read_avg'] / 1000 for w in wl_list()]
        p99 = [d[(w, p)]['read_p99'] / 1000 for w in wl_list()]
        ax.bar(px, avg, bw, color=COLOR[p], hatch=HATCH[p], edgecolor='white', linewidth=0.3, label=LABEL[p] + ' avg')
        ax.scatter(px, p99, marker='_', s=40, color='black', linewidths=1.0, zorder=3)
        ax.vlines(px, avg, p99, colors=COLOR[p], linewidth=0.6, alpha=0.8)
    ax.set_yscale('log')
    finish(ax, xs, labels, seps, gcent, 'Read latency (ms)')
    h, l = ax.get_legend_handles_labels()
    h.append(Line2D([], [], marker='_', ls='', color='black', markersize=8, markeredgewidth=1.0)); l.append('p99')
    ax.legend(h, l, ncol=5, fontsize=6.5, loc='upper center', bbox_to_anchor=(0.5, 1.33), frameon=False, handlelength=1.4)
    fig.tight_layout(pad=0.3)
    save(fig, 'fig_readlat')


def fig_waf(d):
    sel = [('Alibaba 1', 'Ali-high'), ('Alibaba 3', 'Ali-low'), ('SSD', 'SSD'), ('YCSB-A', 'YCSB-A'), ('YCSB-B', 'YCSB-B')]
    pols = ['page4k', 'proposed', 'dftl', 'leaftl', 'iurmw']
    fig, ax = plt.subplots(figsize=(3.4, 1.8))
    nb = len(pols); bw = 0.84 / nb
    for i, p in enumerate(pols):
        ax.bar([k + (i - (nb - 1) / 2) * bw for k in range(len(sel))], [d[(w, p)]['waf'] for w, _ in sel], bw,
               color=COLOR[p], hatch=HATCH[p], edgecolor='white', linewidth=0.3, label=LABEL[p])
    ax.set_xticks(range(len(sel))); ax.set_xticklabels([l for _, l in sel], fontsize=7)
    ax.set_ylabel('Device WAF', fontsize=8)
    ax.grid(axis='y', alpha=0.3, lw=0.5); ax.set_axisbelow(True)
    ax.legend(ncol=3, fontsize=6, loc='upper center', bbox_to_anchor=(0.5, 1.32), frameon=False, handlelength=1.2)
    fig.tight_layout(pad=0.3)
    save(fig, 'fig_waf')


def save(fig, name):
    os.makedirs(OUT, exist_ok=True)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/{name}.{ext}', dpi=200)
    plt.close(fig)


if __name__ == '__main__':
    d = load()
    fig_throughput(d); fig_readlat(d); fig_waf(d)
    print('wrote', OUT)
