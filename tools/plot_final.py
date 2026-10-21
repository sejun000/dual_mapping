#!/usr/bin/env python3
"""Final NVMeVirt matrix (12.5% mapping DRAM): per-workload throughput, WAF and read/write latency (avg, p90, p99).

Sources (results/final_b12):
  apps/<wl>/<policy>/summary.txt      throughput, dev_waf, block-layer blk_{read,write}_lat_us_{mean,p90,p99}
  trace_<t>/<policy>/summary.json     replayer read/write latency; trace_<t>/SUMMARY.txt IOPS and WAF
                                      (page4k for ali1/ali2/ali3/ssd from results/nvmevirt/trace_<t>: budget-independent)
  fio/<policy>_r<R>_s<S>/mixbs.json   fio clat (mean, p90, p99); summary.txt total IOPS and WAF
Writes results/final_b12/figs/{data.csv, <metric>.png, <metric>.pdf}.
"""
import csv, json, math, os, re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = '/home/sejun000/dual_mapping/results'
F = ROOT + '/final_b12'
OUT = F + '/figs'
POL = ['page4k', 'proposed', 'dftl', 'leaftl', 'fast', 'mu', 'iurmw']
LABEL = {'page4k': 'page4k (100%)', 'proposed': 'proposed', 'dftl': 'DFTL', 'leaftl': 'LeaFTL', 'fast': 'FAST',
         'mu': 'μ-FTL', 'iurmw': 'IU+RMW (32K)'}
COLOR = {'page4k': '#888780', 'proposed': '#534AB7', 'dftl': '#1D9E75', 'leaftl': '#D85A30', 'fast': '#BA7517',
         'mu': '#D4537E', 'iurmw': '#378ADD'}
APP_ALIAS = {'page4k': 'page4k', 'proposed': 'proposed_b12f', 'dftl': 'dftl_b12', 'leaftl': 'leaftl_b12',
             'fast': 'fast', 'mu': 'mu_b12', 'iurmw': 'iurmw'}
APPS = [('ycsba_d_t64', 'YCSB-A'), ('ycsbb_d_t64', 'YCSB-B'), ('ycsbc_d_t64', 'YCSB-C'), ('ycsbd_d_t64', 'YCSB-D'),
        ('ycsbe_d_t64', 'YCSB-E'), ('ycsbfstd_d_t64', 'YCSB-F'), ('tpcc_d_t64', 'TPC-C'), ('vmhot1g', 'Varmail')]
TRACES = [('ali1', 'Alibaba 1'), ('ali2', 'Alibaba 2'), ('ali3', 'Alibaba 3'), ('ssd', 'SSD'), ('spfresh', 'SPFresh'),
          ('kv_w1', 'KV cache')]
FIO = [(r, s, f'fio R{r}:W{100 - r}\n4K:128K={s}:{100 - s}') for r in (50, 70, 90) for s in (70, 50, 30)]
METRICS = ['throughput', 'waf', 'read_avg', 'read_p90', 'read_p99', 'write_avg', 'write_p90', 'write_p99']
NAN = float('nan')


def kv(path):
    return dict(l.strip().split('=', 1) for l in open(path) if '=' in l)


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return NAN


def app_row(wl, pol):
    s = kv(f'{F}/apps/{wl}/{APP_ALIAS[pol]}/summary.txt')
    g = lambda k: fnum(s.get(k))
    return {'throughput': g('throughput'), 'waf': g('dev_waf'),
            'read_avg': g('blk_read_lat_us_mean'), 'read_p90': g('blk_read_lat_us_p90'), 'read_p99': g('blk_read_lat_us_p99'),
            'write_avg': g('blk_write_lat_us_mean'), 'write_p90': g('blk_write_lat_us_p90'), 'write_p99': g('blk_write_lat_us_p99')}


def trace_row(t, pol):
    base = f'{ROOT}/nvmevirt/trace_{t}' if pol == 'page4k' and t in ('ali1', 'ali2', 'ali3', 'ssd') else f'{F}/trace_{t}'
    line = next(l for l in open(base + '/SUMMARY.txt') if l.startswith(pol + ' '))
    iops = float(re.search(r'IOPS=\s*(\d+)', line).group(1))
    waf = float(re.search(r'WAF=\s*([\d.]+)', line).group(1))
    j = json.load(open(f'{base}/{pol}/summary.json'))
    r, w = j['read_lat_us'], j['write_lat_us']
    return {'throughput': iops, 'waf': waf, 'read_avg': r['mean'], 'read_p90': r['p90'], 'read_p99': r['p99'],
            'write_avg': w['mean'], 'write_p90': w['p90'], 'write_p99': w['p99']}


def fio_row(rd, s, pol):
    d = f'{F}/fio/{pol}_r{rd}_s{s}'
    t = open(d + '/summary.txt').read()
    j = json.load(open(d + '/mixbs.json'))['jobs'][0]
    c = lambda k: j[k]['clat_ns']
    pc = lambda k, q: c(k)['percentile'][q] / 1e3
    return {'throughput': float(re.search(r'total IOPS=(\d+)', t).group(1)),
            'waf': float(re.search(r'WAF=([\d.]+)', t).group(1)),
            'read_avg': c('read')['mean'] / 1e3, 'read_p90': pc('read', '90.000000'), 'read_p99': pc('read', '99.000000'),
            'write_avg': c('write')['mean'] / 1e3, 'write_p90': pc('write', '90.000000'), 'write_p99': pc('write', '99.000000')}


def collect():
    groups = [('Applications (block-layer latency)', [(lab, lambda p, w=w: app_row(w, p)) for w, lab in APPS]),
              ('Real traces (QD32 replay)', [(lab, lambda p, t=t: trace_row(t, p)) for t, lab in TRACES]),
              ('Raw fio, uniform random, QD32', [(lab, lambda p, r=r, s=s: fio_row(r, s, p)) for r, s, lab in FIO])]
    data = []
    for gname, wls in groups:
        for lab, fn in wls:
            row = {}
            for p in POL:
                try:
                    row[p] = fn(p)
                except Exception as e:
                    print('missing', gname, lab, p, e)
                    row[p] = {m: NAN for m in METRICS}
            data.append((gname, lab, row))
    return data


def plot(data, metric, title, ylabel, logy, norm=False):
    gnames = []
    for g, _, _ in data:
        if g not in gnames:
            gnames.append(g)
    fig, axes = plt.subplots(len(gnames), 1, figsize=(15, 4.2 * len(gnames)))
    nb = len(POL); bw = 0.8 / nb
    for ax, g in zip(axes, gnames):
        rows = [(lab, row) for gg, lab, row in data if gg == g]
        for i, p in enumerate(POL):
            ys = []
            for _, row in rows:
                v = row[p][metric]
                if norm:
                    b = row['page4k'][metric]
                    v = v / b if b and not math.isnan(b) else NAN
                ys.append(v)
            xs = [k + (i - (nb - 1) / 2) * bw for k in range(len(rows))]
            ax.bar(xs, [0 if math.isnan(y) else y for y in ys], bw, color=COLOR[p], label=LABEL[p],
                   edgecolor='white', linewidth=0.3)
            for x, y in zip(xs, ys):
                if math.isnan(y):   # no data (e.g. Varmail: reads are served by the page cache, none reach the device)
                    ax.text(x, 0.02, 'n/a', ha='center', va='bottom', fontsize=6, rotation=90, color='#888780',
                            transform=ax.get_xaxis_transform())
        if norm:
            ax.axhline(1.0, color='#444441', lw=0.8, ls='--')
        ax.set_xticks(range(len(rows)))
        ax.set_xticklabels([lab for lab, _ in rows], fontsize=9)
        if logy:
            ax.set_yscale('log')
        ax.set_ylabel(ylabel, fontsize=10)
        ax.set_title(g, fontsize=11, loc='left')
        ax.grid(axis='y', alpha=0.3)
        ax.set_axisbelow(True)
    axes[0].legend(ncol=len(POL), fontsize=9, loc='lower left', bbox_to_anchor=(0, 1.12), frameon=False)
    fig.suptitle(title + '  (NVMeVirt, mapping DRAM 12.5% for all low-DRAM FTLs; page4k = 100%)', fontsize=12, y=0.995)
    fig.tight_layout()
    for ext in ('png', 'pdf'):
        fig.savefig(f'{OUT}/{metric}.{ext}', dpi=130)
    plt.close(fig)


def main():
    os.makedirs(OUT, exist_ok=True)
    data = collect()
    with open(OUT + '/data.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['group', 'workload', 'policy'] + METRICS)
        for g, lab, row in data:
            for p in POL:
                w.writerow([g, lab.replace('\n', ' '), p] + [row[p][m] for m in METRICS])
    plot(data, 'throughput', 'Throughput normalized to page4k', 'throughput / page4k', False, norm=True)
    plot(data, 'waf', 'Device write amplification', 'WAF (log)', True)
    for side in ('read', 'write'):
        for q, qn in (('avg', 'average'), ('p90', 'p90'), ('p99', 'p99')):
            plot(data, f'{side}_{q}', f'{side.capitalize()} latency, {qn}', f'{side} latency {q} (us, log)', True)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
