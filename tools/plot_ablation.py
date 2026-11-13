#!/usr/bin/env python3
"""DAC ablation figure, same style as tools/plot_eval.py (black bar edges, hatches, DUET saturated).
Build-up from Basic (nsfm: no spill, fine GC always merges, fine area fixed at its cap): + coarse-or-fine victim selection (nsm),
+ copy-or-merge decision (ns), + spill/load (= DUET). Normalized to Basic.
DUET = results/final_vbm (same runs as the main figures), the others = results/final_ablation.
avg. fine pages (smp_* counters of the instrumented module) are kept in the CSV only (DUET from results/final_stats).
Outputs results/paper_dac/fig_ablation.{pdf,png} (also copied to paper/figs) and ablation_data.csv."""
import csv, json, os, re, shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

B = '/home/sejun000/dual_mapping/results'
OUT, PAPER = B + '/paper_dac', '/home/sejun000/dual_mapping/paper/figs'
VAR = ['nsfm', 'nsm', 'ns', 'ck', 'full']   # (2026-11-12, user) coarse-or-fine + copy-or-merge shown as one step (GC decisions)   # (2026-11-12) DUET = spill/load; ck = cuckoo displacement instead of spill/load   # (2026-11-12) + cuckoo displacement (ck), then + spill/load = DUET
LABEL = {'nsfm': 'Basic', 'nsm': '+ Coarse-or-fine', 'ns': '+ Copy-or-merge', 'ck': '+ Cuckoo', 'full': 'Spill/load instead of cuckoo (DUET)'}
COLOR = {'nsfm': '#EEECFB', 'nsm': '#CBC6F3', 'ns': '#9A91E6', 'ck': '#8378E0', 'full': '#3F2FD0'}
HATCH = {'nsfm': '..', 'nsm': '\\\\', 'ns': '//', 'ck': 'xx', 'full': ''}
WL = [('app', 'ycsba_d_t64', 'YCSB-A'), ('app', 'ycsbb_d_t64', 'YCSB-B'), ('trace', 'trace_ali3', 'Alibaba'),
      ('trace', 'replay_a128_r70_s70', '70:30')]   # (2026-11-12, user) SPFresh -> YCSB-B; TPC-C tried and dropped   # (2026-11-12) 32 GB device, results/final_32g
G = B + '/final_32g'
plt.rcParams.update({'font.size': 7, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4, 'axes.linewidth': 0.6})


def dm(path):
    ls = [l for l in open(path) if '[dm ' in l and 'smp_n=' in l]
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', ls[-1])}


def fine_pages(a, b):
    a, b = dm(a), dm(b)
    return (b['smp_fp'] - a['smp_fp']) / (b['smp_n'] - a['smp_n'])


def app(d):
    s = dict(l.strip().split('=', 1) for l in open(f'{d}/summary.txt') if '=' in l)
    assert s['status'] == 'DONE', d
    g = lambda k: float(s[k])
    return {'tput': g('throughput'), 'waf': g('dev_waf'), 'r_avg': g('blk_read_lat_us_mean') / 1e3,
            'r_p99': g('blk_read_lat_us_p99') / 1e3, 'w_p99': g('blk_write_lat_us_p99') / 1e3}


def trace(d):
    j = json.load(open(f'{d}/result.json'))
    return {'tput': j['measured']['iops'], 'waf': j['device']['WAF'], 'r_avg': j['read_lat_us']['mean'] / 1e3,
            'r_p99': j['read_lat_us']['p99'] / 1e3, 'w_p99': j['write_lat_us']['p99'] / 1e3}


# ck = nsk with DRAM timing (50 ns per visited bucket, 4 FTL cores), full = DUET with the same cuckoo step before a spill.
# YCSB-A / SPFresh: results/final_32g/kick_dram (proposedab nskl, proposedst kl). RW 70:30 / Alibaba never fill a bucket
# pair, so ck = nsk (no DRAM timing needed) and full = the main DUET run (0 spills, cuckoo never runs).
KD = {'ycsba_d_t64': True, 'trace_spfresh': True}
def row(kind, key, v):
    if kind == 'app' and v == 'ck':
        d = f'{G}/kick_dram/apps/{key}/proposedab_g32nskl' if key == 'ycsba_d_t64' else f'{G}/ablation/apps/{key}/proposedab_g32nskl'
        r = app(d); r['fp'] = float('nan'); return r
    if kind != 'app' and v == 'ck':
        if key in KD:
            d = f'{G}/kick_dram/' + ('nskl' if v == 'ck' else 'kl') + f'/{key}/' + ('proposedab' if v == 'ck' else 'proposedst')
        else:
            d = f'{G}/ablation/nsk/{key}/proposedab' if v == 'ck' else f'{G}/{key}/proposed'
        r = trace(d); r['fp'] = float('nan'); return r
    if kind == 'app':
        d = f'{G}/apps/{key}/proposed_g32' if v == 'full' else f'{G}/ablation/apps/{key}/proposedab_g32{v}'
        r = app(d)
        try: r['fp'] = fine_pages(f'{d}/dm_start.txt', f'{d}/dm_end.txt')
        except Exception: r['fp'] = float('nan')   # DUET main runs are not the stats build
    else:
        d = f'{G}/{key}/proposed' if v == 'full' else f'{G}/ablation/{v}/{key}/proposedab'
        r = trace(d)
        try: r['fp'] = fine_pages(f'{d}/stats_warm_end.txt', f'{d}/stats_end.txt')
        except Exception: r['fp'] = float('nan')
    return r


def row_old(kind, key, v):
    if kind == 'app':
        d = f'{B}/final_vbm/apps/{key}/proposed_vb' if v == 'full' else f'{B}/final_ablation/apps/{key}/proposedab_{v}'
        s = f'{B}/final_stats/apps/{key}/proposedst_vb' if v == 'full' else d
        r = app(d); r['fp'] = fine_pages(f'{s}/dm_start.txt', f'{s}/dm_end.txt')
    else:
        d = f'{B}/final_vbm/{key}/proposed' if v == 'full' else f'{B}/final_ablation/{v}/{key}/proposedab'
        s = f'{B}/final_stats/{key}/proposedst' if v == 'full' else d
        r = trace(d); r['fp'] = fine_pages(f'{s}/stats_warm_end.txt', f'{s}/stats_end.txt')
    return r


def main():
    data = [(lab, {v: row(kind, key, v) for v in VAR}) for kind, key, lab in WL]
    with open(OUT + '/ablation_data.csv', 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['workload', 'variant', 'tput', 'waf', 'r_avg_ms', 'r_p99_ms', 'w_p99_ms', 'fine_pages'])
        for lab, r in data:
            for v in VAR:
                w.writerow([lab, v] + [round(r[v][m], 4) for m in ('tput', 'waf', 'r_avg', 'r_p99', 'w_p99', 'fp')])

    panels = [('tput', 'Norm.\nthroughput', True, (0, 1.9), [0, 0.5, 1, 1.5], '{:.2f}'),
              ('waf', 'WA', False, None, None, '{:.1f}'),
              ('r_p99', 'p99 read\n(ms)', False, None, None, '{:.1f}'),
              ('w_p99', 'p99 write\n(ms)', False, None, None, '{:.1f}')]
    fig, axs = plt.subplots(2, 2, figsize=(3.4, 1.28))   # was 1.95, 1.4 (2026-11-13)
    nb = len(VAR); bw = 0.84 / nb; xs = list(range(len(data)))
    for ax, (m, yl, norm, ylim, yt, fmt) in zip(axs.flat, panels):
        for i, v in enumerate(VAR):
            px = [x + (i - (nb - 1) / 2) * bw for x in xs]
            ys = [r[v][m] / (r['nsfm'][m] if norm else 1) for _, r in data]
            ax.bar(px, ys, bw, color=COLOR[v], hatch=HATCH[v], edgecolor='black', linewidth=0.35)
        if norm:
            ax.axhline(1.0, color='#444441', lw=0.6, ls='--', zorder=0)
        if m in ('r_p99', 'w_p99'):
            ax.set_yscale('log')
        if ylim: ax.set_ylim(*ylim)
        if yt: ax.set_yticks(yt)
        if m in ('r_p99', 'w_p99'):
            from matplotlib.ticker import FuncFormatter
            ax.yaxis.set_major_formatter(FuncFormatter(lambda t, _: f'{t:g}')); ax.minorticks_off()
        ax.set_xticks(xs); ax.set_xticklabels([l for l, _ in data], fontsize=4.6)
        ax.set_xlim(-0.5, len(xs) - 0.5)
        ax.set_ylabel(yl, fontsize=5.6, labelpad=1.5, linespacing=0.9)
        ax.tick_params(axis='y', labelsize=5.6, pad=1); ax.tick_params(axis='x', pad=1, length=0)
        ax.grid(axis='y', alpha=0.3, lw=0.5); ax.set_axisbelow(True)
    fig.align_ylabels(axs[:, 0]); fig.align_ylabels(axs[:, 1])
    fig.tight_layout(pad=0.15, h_pad=0.4, w_pad=0.6); fig.subplots_adjust(top=0.78)   # was 0.86, 0.80 (2026-11-13: same legend height)
    h = [Patch(fc=COLOR[v], hatch=HATCH[v], ec='black', lw=0.35, label=LABEL[v]) for v in VAR]
    fig.legend(handles=h, ncol=3, fontsize=5.5, loc='upper center', bbox_to_anchor=(0.5, 1.02), frameon=False,
               handlelength=1.5, handleheight=0.9, columnspacing=0.8, handletextpad=0.4)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/fig_ablation.{ext}', dpi=250, bbox_inches='tight', pad_inches=0.02)
    shutil.copy(f'{OUT}/fig_ablation.pdf', f'{PAPER}/fig_ablation.pdf')
    for lab, r in data:
        print(lab, {v: {m: round(r[v][m], 2) for m in ('tput', 'waf', 'r_p99', 'w_p99', 'fp')} for v in VAR})


if __name__ == '__main__':
    main()
