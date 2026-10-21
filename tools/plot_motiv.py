#!/usr/bin/env python3
"""DAC Motivation figure (x labels carry each FTL's mapping DRAM in MiB; DUET included for reference) (results/paper_dac/fig_motiv.{pdf,png}): raw I/O, one config (MOTIV_CFG, default r70_s70 = 70% reads, 4K:128K = 70:30; 128K-aligned uniform random,
70% of requests 4 KiB, 50% reads, QD32 replay; results/final_rp_a128), 1/8 mapping DRAM, OP 7%. Single-column 2x2:
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
CFG = os.environ.get('MOTIV_CFG', 'r70_s70')
D = B + '/final_rp_a128/replay_a128_' + CFG
OUT = B + '/paper_dac'
LABEL = {'page4k': 'Page', 'dftl': 'DFTL', 'leaftl': 'LeaFTL', 'fast': 'FAST', 'iurmw': 'IU+RMW', 'proposed': 'DUET'}
COLOR = {'page4k': '#B4B2A9', 'dftl': '#1D9E75', 'leaftl': '#D85A30', 'fast': '#BA7517', 'iurmw': '#378ADD', 'proposed': '#534AB7'}
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
    ax.set_ylabel(ylabel, fontsize=6.8); ax.set_title(title, fontsize=7, pad=2)
    ax.grid(axis='y', alpha=0.3, lw=0.5); ax.set_axisbelow(True)


def main():
    s = {p: load(p) for p in LABEL}
    fig, axs = plt.subplots(2, 2, figsize=(3.4, 2.9))
    od, hy = ['page4k', 'dftl', 'leaftl', 'proposed'], ['page4k', 'fast', 'iurmw', 'proposed']
    mem = {p: s[p]['mem'] for p in s}
    # FAST: report provisioned mapping DRAM (block map + hash for the 15 log blocks its cap allows: NLB*4 + 15*P*8/0.7),
    # not the live-entry snapshot (mem_bytes), like the statically sized tables of the other FTLs.
    P, NLB, nlb = 67584, 348, 15
    mem['fast'] = (NLB * 4 + nlb * ((P * 80 + 6) // 7)) / 2**20
    panel(axs[0][0], od, [s[p]['fr'] for p in od], ylabel='Flash reads', title='(a) Flash reads per read', mem=mem)
    axs[0][0].set_ylim(0, 2.2)
    panel(axs[0][1], od, [s[p]['r_avg'] for p in od], ylabel='Avg. read lat. (ms)',
          title='(b) Read latency', fmt='{:.1f}', mem=mem)
    axs[0][1].set_ylim(0, 6.5)
    panel(axs[1][1], hy, [s[p]['w_avg'] for p in hy], ylabel='Avg. write lat. (ms)',
          title='(d) Write latency', fmt='{:.1f}', mem=mem)
    axs[1][1].set_ylim(0, 8.5)
    panel(axs[1][0], hy, [s[p]['waf'] for p in hy], ylabel='Device WAF', title='(c) Write amplification', fmt='{:.1f}', mem=mem)
    axs[1][0].set_ylim(0, 31)
    fig.tight_layout(pad=0.3, h_pad=0.6, w_pad=0.6)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/fig_motiv.{ext}', dpi=250, bbox_inches='tight', pad_inches=0.02)
    print('wrote', OUT + '/fig_motiv.{pdf,png}', {p: {k: round(v, 2) for k, v in s[p].items()} for p in s})


if __name__ == '__main__':
    main()
