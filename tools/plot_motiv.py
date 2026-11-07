#!/usr/bin/env python3
"""DAC Motivation figure. Default (MOTIV_CFG=gmean): geomean over the 10 evaluation workloads of each metric normalized
to Page (results/paper_dac/eval_data.csv from tools/plot_eval.py), no DRAM labels. MOTIV_CFG=r70_s70 etc.: one raw I/O config (x labels carry each FTL's mapping DRAM in MB; DUET included for reference) (results/paper_dac/fig_motiv.{pdf,png}): raw I/O, one config (MOTIV_CFG, default r70_s70 = 70% reads, 4K:128K = 70:30; 128K-aligned uniform random,
70% of requests 4 KB, 50% reads, QD32 replay; results/final_rp_a128), 1/8 mapping DRAM, OP 7%. Single-column 2x2:
 (a) flash reads per host read page = 1 + mapping-page reads per host read (LeaFTL misprediction reads not counted);
 (b) average read latency, (c) device WAF, (d) average write latency.
(a)/(b): page-level vs on-demand (DFTL, LeaFTL); (c)/(d): page-level vs block-level hybrid (FAST) and large IU (IU+RMW).
"""
import json
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

B = '/home/sejun000/dual_mapping/results'
import os
CFG = os.environ.get('MOTIV_CFG', 'gmean')   # 'gmean' = geomean over the 10 evaluation workloads (eval_data.csv)
D = B + '/final_rp_a128/replay_a128_' + CFG
OUT = B + '/paper_dac'
LABEL = {'page4k': 'Page', 'dftl': 'DFTL', 'leaftl': 'LeaFTL', 'fast': 'FAST', 'iurmw': 'IU+RMW', 'proposed': 'DUET'}
COLOR = {'page4k': '#DDDCD6', 'dftl': '#B3E0CC', 'leaftl': '#F5C2AE', 'fast': '#EED6AA', 'iurmw': '#BBD7F3', 'proposed': '#3F2FD0'}   # pastel baselines, saturated DUET
HATCH = {'page4k': '', 'dftl': '//', 'leaftl': '\\\\', 'fast': '..', 'iurmw': '--', 'proposed': ''}
plt.rcParams.update({'font.size': 7, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4, 'axes.linewidth': 0.6})


def load(p):
    j = json.load(open(f'{D}/{p}/result.json'))
    d, r, w = j['device'], j['read_lat_us'], j['write_lat_us']
    return {'fr': 1 + d['map_reads'] / max(1, d['host_read_pages']), 'waf': d['WAF'], 'mem': d['mem_bytes'] / 2**20,
            'r_avg': r['mean'] / 1e3, 'r_p99': r['p99'] / 1e3, 'w_avg': w['mean'] / 1e3, 'w_p99': w['p99'] / 1e3}


def panel(ax, pols, vals, p99=None, ylabel='', title='', fmt='{:.2f}', mem=None):
    x = list(range(len(pols)))
    bars = ax.bar(x, vals, 0.62, color=[COLOR[p] for p in pols], edgecolor='black', linewidth=0.6)
    for b, p in zip(bars, pols):
        b.set_hatch(HATCH[p])
    if p99:
        ax.scatter(x, p99, marker='_', s=60, color='black', linewidths=1.1, zorder=3, label='p99')
    for k, v in enumerate(vals):
        ax.text(k, v, fmt.format(v), ha='center', va='bottom', fontsize=5.3)
    ax.set_xticks(x)
    ax.set_xticklabels([f'{LABEL[p]}\n' + (f'{mem[p]:.0f}MB' if mem[p] >= 20 else f'{mem[p]:.1f}MB') if mem else LABEL[p] for p in pols], fontsize=5.6)
    ax.set_ylabel(ylabel, fontsize=6.3, labelpad=1.5, linespacing=0.9); ax.set_xlabel(title, fontsize=6.8, labelpad=2.5)   # sub-caption below
    ax.grid(axis='y', alpha=0.3, lw=0.5); ax.set_axisbelow(True)


def gmean_all():
    # geomean over the 10 evaluation workloads (tools/plot_eval.py -> eval_data.csv) of each metric / Page's
    import csv, math
    rows = {}
    for r in csv.DictReader(open(OUT + '/eval_data.csv')):
        rows.setdefault(r['workload'], {})[r['policy']] = r
    col = {'fr': 'fr', 'waf': 'waf', 'r_avg': 'r_avg_ms', 'w_avg': 'w_avg_ms'}
    gm = lambda v: math.exp(sum(map(math.log, v)) / len(v))
    # WA is the geomean of the absolute device WA (not normalized), so panel (c) is labelled plainly 'WA'.
    # Ratios to Page are unchanged: geomean(a / b) = geomean(a) / geomean(b) over the same workloads.
    return {p: {m: gm([float(w[p][c]) / (1 if m == 'waf' else float(w['page4k'][c])) for w in rows.values()])
                for m, c in col.items()} for p in LABEL}


def main():
    if CFG == 'gmean':
        return main_gmean()
    s = {p: load(p) for p in LABEL}
    fig, axs = plt.subplots(2, 2, figsize=(3.4, 2.15))
    od, hy = ['page4k', 'dftl', 'leaftl', 'proposed'], ['page4k', 'fast', 'iurmw', 'proposed']
    mem = {p: s[p]['mem'] for p in s}
    # every bounded FTL is labelled with the common budget (B12 = 1/8 of the page-level table)
    for p in ('dftl', 'leaftl', 'fast', 'iurmw', 'proposed'):
        mem[p] = 11.2
    panel(axs[0][0], od, [s[p]['fr'] for p in od], ylabel='Flash reads', title='(a) Flash reads per read', mem=mem)
    axs[0][0].set_ylim(0, 2.2)
    panel(axs[0][1], od, [s[p]['r_avg'] for p in od], ylabel='Avg. read (ms)',
          title='(b) Read latency', fmt='{:.1f}', mem=mem)
    axs[0][1].set_ylim(0, 6.5)
    panel(axs[1][1], hy, [s[p]['w_avg'] for p in hy], ylabel='Avg. write (ms)',
          title='(d) Write latency', fmt='{:.1f}', mem=mem)
    axs[1][1].set_ylim(0, 8.5)
    panel(axs[1][0], hy, [s[p]['waf'] for p in hy], ylabel='Device WA', title='(c) Write amplification', fmt='{:.1f}', mem=mem)
    axs[1][0].set_ylim(0, 31)
    for ax in axs.flat:
        ax.tick_params(axis='y', labelsize=5.6, pad=1); ax.tick_params(axis='x', pad=1)
    fig.tight_layout(pad=0.2, h_pad=0.9, w_pad=0.4)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/fig_motiv.{ext}', dpi=250, bbox_inches='tight', pad_inches=0.02)
    print('wrote', OUT + '/fig_motiv.{pdf,png}', {p: {k: round(v, 2) for k, v in s[p].items()} for p in s})


def main_gmean():
    s = gmean_all()
    fig, axs = plt.subplots(2, 2, figsize=(3.4, 1.7))
    od, hy = ['page4k', 'dftl', 'leaftl', 'proposed'], ['page4k', 'fast', 'iurmw', 'proposed']
    panel(axs[0][0], od, [s[p]['fr'] for p in od], ylabel='Flash reads', title='(a) Flash reads per read')
    axs[0][0].set_ylim(0, 1.75)
    panel(axs[0][1], od, [s[p]['r_avg'] for p in od], ylabel='Norm. latency', title='(b) Avg. read latency')
    axs[0][1].set_ylim(0, 2.9)
    panel(axs[1][0], hy, [s[p]['waf'] for p in hy], ylabel='WA', title='(c) Write amplification', fmt='{:.1f}')
    axs[1][0].set_ylim(0, 1.25 * max(s[p]['waf'] for p in hy))
    panel(axs[1][1], hy, [s[p]['w_avg'] for p in hy], ylabel='Norm. latency', title='(d) Avg. write latency')
    axs[1][1].set_ylim(0, 3.2)
    for ax in axs.flat:
        ax.tick_params(axis='y', labelsize=5.6, pad=1); ax.tick_params(axis='x', pad=1)
    fig.tight_layout(pad=0.2, h_pad=0.9, w_pad=0.4)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/fig_motiv.{ext}', dpi=250, bbox_inches='tight', pad_inches=0.02)
    print('wrote', OUT + '/fig_motiv.{pdf,png}', {p: {k: round(v, 2) for k, v in s[p].items()} for p in s})


if __name__ == '__main__':
    main()
