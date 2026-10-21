#!/usr/bin/env python3
"""Paper figures/tables for the final NVMeVirt run (results/final_rp): 12.5% mapping DRAM, paced background NAND ops,
host-read priority, LeaFTL with a 1 MiB sort batch. 11 workloads x 7 policies.

Latency: apps = block-layer latency (bpftrace), traces = replayer completion latency, fio = fio clat.
Outputs results/final_rp/paper/{data.csv, tables.md, fig_throughput, fig_readlat, fig_writelat, fig_waf}.{pdf,png}
"""
import csv, json, math, os, re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

R = '/home/sejun000/dual_mapping/results/final_rp'
OUT = R + '/paper'
POL = ['page4k', 'proposed', 'dftl', 'leaftl', 'fast', 'mu', 'iurmw']
LABEL = {'page4k': 'Page (100% DRAM)', 'proposed': 'Proposed', 'dftl': 'DFTL', 'leaftl': 'LeaFTL', 'fast': 'FAST',
         'mu': 'μ-FTL', 'iurmw': 'IU+RMW'}
COLOR = {'page4k': '#B4B2A9', 'proposed': '#534AB7', 'dftl': '#1D9E75', 'leaftl': '#D85A30', 'fast': '#BA7517',
         'mu': '#D4537E', 'iurmw': '#378ADD'}
HATCH = {'page4k': '', 'proposed': '', 'dftl': '//', 'leaftl': '\\\\', 'fast': '..', 'mu': 'xx', 'iurmw': '--'}
APP_ALIAS = {'page4k': 'page4k', 'proposed': 'proposed_b12f', 'dftl': 'dftl_b12', 'leaftl': 'leaftl_b12',
             'fast': 'fast', 'mu': 'mu_b12', 'iurmw': 'iurmw'}
GROUPS = [
    ('Real traces', [('trace', 'ali3', 'Ali-low'), ('trace', 'spfresh', 'SPFresh')]),
    ('Applications', [('app', 'ycsba_d_t64', 'YCSB-A'), ('app', 'ycsbb_d_t64', 'YCSB-B'), ('app', 'ycsbc_d_t64', 'YCSB-C'),
                      ('app', 'ycsbe_d_t64', 'YCSB-E'), ('app', 'tpcc_d_t64', 'TPC-C')]),
    ('Raw fio (uniform random)', [('fio', (50, 70), 'R50\n4K-hv'), ('fio', (50, 30), 'R50\n128K-hv'),
                                  ('fio', (90, 70), 'R90\n4K-hv'), ('fio', (90, 30), 'R90\n128K-hv')]),
]
METRICS = ['throughput', 'waf', 'read_avg', 'read_p99', 'write_avg', 'write_p99']
NAN = float('nan')
plt.rcParams.update({'font.size': 8, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4, 'axes.linewidth': 0.6})


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return NAN


def app_row(wl, pol):
    s = dict(l.strip().split('=', 1) for l in open(f'{R}/apps/{wl}/{APP_ALIAS[pol]}/summary.txt') if '=' in l)
    if s.get('status') != 'DONE':
        raise ValueError('not done')
    g = lambda k: fnum(s.get(k))
    return {'throughput': g('throughput'), 'waf': g('dev_waf'),
            'read_avg': g('blk_read_lat_us_mean') / 1e3, 'read_p99': g('blk_read_lat_us_p99') / 1e3,
            'write_avg': g('blk_write_lat_us_mean') / 1e3, 'write_p99': g('blk_write_lat_us_p99') / 1e3}


def trace_row(t, pol):
    line = next(l for l in open(f'{R}/trace_{t}/SUMMARY.txt') if l.startswith(pol + ' '))
    j = json.load(open(f'{R}/trace_{t}/{pol}/summary.json'))
    r, w = j['read_lat_us'], j['write_lat_us']
    return {'throughput': float(re.search(r'IOPS=\s*(\d+)', line).group(1)),
            'waf': float(re.search(r'WAF=\s*([\d.]+)', line).group(1)),
            'read_avg': r['mean'] / 1e3, 'read_p99': r['p99'] / 1e3, 'write_avg': w['mean'] / 1e3, 'write_p99': w['p99'] / 1e3}


def fio_row(cfg, pol):
    rd, s = cfg
    d = f'{R}/fio/{pol}_r{rd}_s{s}'
    t = open(d + '/summary.txt').read()
    j = json.load(open(d + '/mixbs.json'))['jobs'][0]
    c = lambda k: j[k]['clat_ns']
    return {'throughput': float(re.search(r'total IOPS=(\d+)', t).group(1)), 'waf': float(re.search(r'WAF=([\d.]+)', t).group(1)),
            'read_avg': c('read')['mean'] / 1e6, 'read_p99': c('read')['percentile']['99.000000'] / 1e6,
            'write_avg': c('write')['mean'] / 1e6, 'write_p99': c('write')['percentile']['99.000000'] / 1e6}


FN = {'app': app_row, 'trace': trace_row, 'fio': fio_row}


def collect():
    data = []
    for g, wls in GROUPS:
        for kind, key, lab in wls:
            row = {}
            for p in POL:
                try:
                    row[p] = FN[kind](key, p)
                except Exception as e:
                    print('missing', lab.replace('\n', ' '), p, e)
                    row[p] = {m: NAN for m in METRICS}
            data.append((g, lab, row))
    return data


def layout(data):
    xs, labels, seps, gcent = [], [], [], []
    x = 0; prev = None; start = 0
    for g, lab, _ in data:
        if prev is not None and g != prev:
            gcent.append(((start + x - 1) / 2, prev)); seps.append(x - 0.5 + 0.35); x += 0.7; start = x
        if prev is None:
            start = x
        xs.append(x); labels.append(lab); x += 1; prev = g
    gcent.append(((start + x - 1) / 2, prev))
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


def bars(data, metric, fname, ylabel, logy=False, norm=False, p99=None, ylim=None):
    xs, labels, seps, gcent = layout(data)
    fig, ax = plt.subplots(figsize=(7.0, 2.3))
    nb = len(POL); bw = 0.84 / nb
    for i, p in enumerate(POL):
        px = [x + (i - (nb - 1) / 2) * bw for x in xs]
        ys = []
        for _, _, row in data:
            v = row[p][metric]
            if norm:
                b = row['page4k'][metric]; v = v / b if b else NAN
            ys.append(v)
        ax.bar(px, [0 if math.isnan(y) else y for y in ys], bw, color=COLOR[p], hatch=HATCH[p], edgecolor='white',
               linewidth=0.3, label=LABEL[p])
        if p99:
            ps = [row[p][p99] for _, _, row in data]
            ax.scatter(px, ps, marker='_', s=22, color='black', linewidths=0.9, zorder=3)
    if norm:
        ax.axhline(1.0, color='#444441', lw=0.6, ls='--')
    if logy:
        ax.set_yscale('log')
    if ylim:
        ax.set_ylim(*ylim)
    finish(ax, xs, labels, seps, gcent, ylabel)
    h, l = ax.get_legend_handles_labels()
    if p99:
        h.append(Line2D([], [], marker='_', ls='', color='black', markersize=7, markeredgewidth=0.9)); l.append('p99')
    ax.legend(h, l, ncol=8, fontsize=6.3, loc='upper center', bbox_to_anchor=(0.5, 1.30), frameon=False,
              handlelength=1.3, columnspacing=0.8)
    fig.tight_layout(pad=0.3)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/{fname}.{ext}', dpi=200)
    plt.close(fig)


def tables(data):
    def fmt(v, kind):
        if math.isnan(v):
            return '-'
        if kind == 'thr':
            return f'{v/1000:.1f}K' if v >= 1000 else f'{v:.0f}'
        return f'{v:.2f}'
    lines = []
    for metric, title, kind in (('throughput', 'Throughput (ops/s or IOPS; ratio to page4k in parentheses)', 'thr'),
                                ('read_avg', 'Read latency average (ms)', 'lat'), ('read_p99', 'Read latency p99 (ms)', 'lat'),
                                ('write_avg', 'Write latency average (ms)', 'lat'), ('waf', 'Device WAF', 'lat')):
        lines.append(f'\n### {title}\n')
        lines.append('| workload | ' + ' | '.join(LABEL[p] for p in POL) + ' |')
        lines.append('|---' * (len(POL) + 1) + '|')
        for _, lab, row in data:
            cells = []
            for p in POL:
                v = row[p][metric]
                c = fmt(v, kind)
                if metric == 'throughput' and p != 'page4k' and not math.isnan(v):
                    c += f' ({v / row["page4k"][metric]:.2f})'
                cells.append(c)
            lines.append(f'| {lab.replace(chr(10), " ")} | ' + ' | '.join(cells) + ' |')
    gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
    lines.append('\n### Throughput geomean vs page4k\n')
    for p in POL[1:]:
        r = [row[p]['throughput'] / row['page4k']['throughput'] for _, _, row in data
             if not math.isnan(row[p]['throughput'])]
        lines.append(f'- {LABEL[p]}: {gm(r):.3f} over {len(r)} workloads')
    open(OUT + '/tables.md', 'w').write('\n'.join(lines) + '\n')


def main():
    os.makedirs(OUT, exist_ok=True)
    data = collect()
    with open(OUT + '/data.csv', 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['group', 'workload', 'policy'] + METRICS)
        for g, lab, row in data:
            for p in POL:
                w.writerow([g, lab.replace('\n', ' '), p] + [row[p][m] for m in METRICS])
    bars(data, 'throughput', 'fig_throughput', 'Throughput\n(norm. to Page)', norm=True)
    bars(data, 'read_avg', 'fig_readlat', 'Read latency (ms)\nbar: avg, dash: p99', logy=True, p99='read_p99')
    bars(data, 'write_avg', 'fig_writelat', 'Write latency (ms)\nbar: avg, dash: p99', logy=True, p99='write_p99')
    bars(data, 'waf', 'fig_waf', 'Device WAF', logy=True)
    tables(data)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
