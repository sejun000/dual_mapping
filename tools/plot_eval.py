#!/usr/bin/env python3
"""DAC Evaluation figures, same style as the Motivation figure (tools/plot_motiv.py): pastel baselines, saturated DUET,
black bar edges, hatches. 1/8 mapping DRAM, OP 7%, NVMeVirt. Outputs results/paper_dac/fig_eval_{read,write,tput}.{pdf,png}
(also copied to paper/figs) and results/paper_dac/eval_data.csv.
 read : (a) flash reads per host read page (1 + mapping-page reads), (b) average and (c) p99 read latency
 write: (a) device WA (log scale), (b) average and (c) p99 write latency
 tput : throughput. Latency and throughput are normalized to Page.
Sources: bounded FTLs (DUET, DFTL, LeaFTL, uFTL) = results/final_vbm (valid-bitmap budget rerun).
CoMap = results/final_comap_tune (tuned rerun, 2026-11-08, was final_vbm).
Page / FAST / IU+RMW (not rerun, see results/nvmevirt/q_vbm.sh) = results/final_rp (traces, apps) and
results/final_rp_a128 (raw I/O replays). Latency: apps = block-layer latency, traces / raw I/O = replayer completion."""
import csv, json, math, shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Patch

B = '/home/sejun000/dual_mapping/results'
VBM, RP, A128 = B + '/final_vbm', B + '/final_rp', B + '/final_rp_a128'
FF = B + '/final_fastfull'   # FAST with cache_bytes=1e9 (map of every log block in DRAM), 2026-11-07
CT = B + '/final_comap_tune'   # CoMap tuned: comap_logfrac_ppm=990000, comap_p0=990000 (q_comap_tune.sh, 2026-11-08)
FAST_FULL = False            # False = FAST-B12 (final_rp / final_rp_a128), the figures before 2026-11-07
OUT = B + '/paper_dac'
PAPER = '/home/sejun000/dual_mapping/paper/figs'
POL = ['page4k', 'dftl', 'leaftl', 'comap', 'fast', 'iurmw', 'proposed']   # uFTL dropped (2026-11-06)
NEW = {'proposed', 'dftl', 'leaftl', 'comap'}
LABEL = {'page4k': 'Page (full DRAM)', 'dftl': 'DFTL', 'leaftl': 'LeaFTL', 'mu': 'μFTL', 'comap': 'CoMap',
         'fast': 'FAST', 'iurmw': 'CoarseIU', 'proposed': 'DUET'}
COLOR = {'page4k': '#DDDCD6', 'dftl': '#B3E0CC', 'leaftl': '#F5C2AE', 'mu': '#F2C9DD', 'comap': '#DCE6B4',
         'fast': '#EED6AA', 'iurmw': '#BBD7F3', 'proposed': '#3F2FD0'}
HATCH = {'page4k': '', 'dftl': '//', 'leaftl': '\\\\', 'mu': 'xx', 'comap': '++', 'fast': '..', 'iurmw': '--',
         'proposed': ''}
WL = [('raw', f'a128_r{r}_s70', f'{r}:{100 - r}') for r in (50, 70, 90)] + \
     [('trace', 'ali3', 'Alibaba'), ('trace', 'spfresh', 'SPFresh'),
      ('app', 'ycsba_d_t64', 'YCSB-A'), ('app', 'ycsbb_d_t64', 'YCSB-B'), ('app', 'ycsbc_d_t64', 'YCSB-C'),
      ('app', 'ycsbe_d_t64', 'YCSB-E'), ('app', 'tpcc_d_t64', 'TPC-C')]   # raw I/O first, 4K:128K = 70:30 only
GROUP = {'trace': 'Real-world traces', 'app': 'Applications', 'raw': 'Synthetic raw I/O (R:W)'}
plt.rcParams.update({'font.size': 7, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4, 'axes.linewidth': 0.6})


def replay(d, p):
    j = json.load(open(f'{d}/{p}/result.json'))
    dv, r, w = j['device'], j['read_lat_us'], j['write_lat_us']
    tput = j['measured']['iops'] if 'iops' in j.get('measured', {}) else None
    if tput is None:
        line = next(l for l in open(f'{d}/SUMMARY.txt') if l.startswith(p + ' '))
        tput = float(line.split('IOPS=')[1].split()[0])
    return {'tput': tput, 'fr': 1 + dv['map_reads'] / max(1, dv['host_read_pages']), 'waf': dv['WAF'],
            'r_avg': r['mean'] / 1e3, 'w_avg': w['mean'] / 1e3, 'r_p99': r['p99'] / 1e3, 'w_p99': w['p99'] / 1e3}


def app(wl, p):
    d, al = (f'{VBM}/apps/{wl}', p + '_vb') if p in NEW else (f'{RP}/apps/{wl}', p)
    if p == 'fast' and FAST_FULL:
        d, al = f'{FF}/apps/{wl}', 'fast_full'
    if p == 'comap':
        d, al = f'{CT}/apps/{wl}', 'comap_vbt'
    s = dict(l.strip().split('=', 1) for l in open(f'{d}/{al}/summary.txt') if '=' in l)
    assert s['status'] == 'DONE', (wl, p)
    g = lambda k: float(s[k])
    return {'tput': g('throughput'), 'fr': 1 + g('dev_map_reads_per_host_read'), 'waf': g('dev_waf'),
            'r_avg': g('blk_read_lat_us_mean') / 1e3, 'w_avg': g('blk_write_lat_us_mean') / 1e3,
            'r_p99': g('blk_read_lat_us_p99') / 1e3, 'w_p99': g('blk_write_lat_us_p99') / 1e3}


def row(kind, key, p):
    if kind == 'app':
        return app(key, p)
    ff = p == 'fast' and FAST_FULL
    if p == 'comap':
        return replay(f'{CT}/{"trace_" if kind == "trace" else "replay_"}{key}', p)
    if kind == 'trace':
        return replay(f'{FF if ff else VBM if p in NEW else RP}/trace_{key}', p)
    return replay(f'{FF if ff else VBM if p in NEW else A128}/replay_{key}', p)


# Read-only workloads: almost no host writes in the measured window (YCSB-C: 92 .. 1.6 M pages across FTLs, PostgreSQL
# hint-bit / checkpoint timing), so WA and write latency are noise. Their write bars are left empty ('Read-only') and
# they are excluded from the write-metric geomeans (2026-11-08).
READ_ONLY = {'YCSB-C'}
WRITE_M = ('waf', 'w_avg', 'w_p99')


def collect():
    return [(kind, lab, {p: row(kind, key, p) for p in POL}) for kind, key, lab in WL]


def xpos():
    xs, x, prev = [], 0.0, None
    for kind, _, _ in WL:
        if prev is not None and kind != prev:
            x += 0.45
        xs.append(x); x += 1; prev = kind
    return xs


def panel(ax, data, metric, ylabel, title, norm=False, clip=None, logy=False, ylim=None, fmt='{:.1f}', xt=True):
    xs = xpos(); nb = len(POL); bw = 0.86 / nb
    skip = [metric in WRITE_M and l in READ_ONLY for _, l, _ in data]
    for i, p in enumerate(POL):
        px = [x + (i - (nb - 1) / 2) * bw for x, sk in zip(xs, skip) if not sk]
        ys = [r[p][metric] / (r['page4k'][metric] if norm else 1) for (_, _, r), sk in zip(data, skip) if not sk]
        bars = ax.bar(px, [min(y, clip) if clip else y for y in ys], bw, color=COLOR[p], hatch=HATCH[p],
                      edgecolor='black', linewidth=0.35)
        if clip:
            for x, y in zip(px, ys):
                if y > clip:
                    ax.text(x, clip * 0.97, fmt.format(y), ha='center', va='top', fontsize=4.2, rotation=90)
    for x, sk in zip(xs, skip):
        if sk:
            ax.text(x, 0.5, 'Read-only', transform=ax.get_xaxis_transform(), ha='center', va='center',
                    fontsize=5.0, color='#444441')
    if norm:
        ax.axhline(1.0, color='#444441', lw=0.6, ls='--', zorder=0)
    if logy:
        ax.set_yscale('log')
    if ylim:
        ax.set_ylim(*ylim)
    seps, prev = [], None
    for x, (kind, _, _) in zip(xs, data):
        if prev is not None and kind != prev:
            seps.append(x - 0.72)
        prev = kind
    for s in seps:
        ax.axvline(s, color='#888780', lw=0.6, ls=':')
    ax.set_xticks(xs)
    ax.set_xticklabels([l for _, l, _ in data] if xt else [], fontsize=5.8, linespacing=0.9)
    ax.set_xlim(xs[0] - 0.55, xs[-1] + 0.55)
    ax.set_ylabel(ylabel, fontsize=6.3, labelpad=1.5)
    if title:
        ax.set_xlabel(title, fontsize=6.8, labelpad=2.5)
    ax.tick_params(axis='y', labelsize=5.6, pad=1); ax.tick_params(axis='x', pad=1, length=0)
    ax.grid(axis='y', alpha=0.3, lw=0.5); ax.set_axisbelow(True)
    return xs


def groups(ax, data, xs):
    start = 0
    for k in range(1, len(data) + 1):
        if k == len(data) or data[k][0] != data[start][0]:
            ax.text((xs[start] + xs[k - 1]) / 2, 1.02, GROUP[data[start][0]], transform=ax.get_xaxis_transform(),
                    ha='center', va='bottom', fontsize=6.3, fontweight='bold')
            start = k


def legend(fig, y):
    h = [Patch(fc=COLOR[p], hatch=HATCH[p], ec='black', lw=0.35, label=LABEL[p]) for p in POL]
    fig.legend(handles=h, ncol=len(POL), fontsize=6.0, loc='upper center', bbox_to_anchor=(0.5, y), frameon=False,
               handlelength=1.6, handleheight=0.9, columnspacing=1.2)


def save(fig, name):
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/{name}.{ext}', dpi=250, bbox_inches='tight', pad_inches=0.02)
    shutil.copy(f'{OUT}/{name}.pdf', f'{PAPER}/{name}.pdf')
    plt.close(fig)


def main():
    data = collect()
    with open(OUT + '/eval_data.csv', 'w', newline='') as f:
        w = csv.writer(f); w.writerow(['workload', 'policy', 'tput', 'fr', 'waf', 'r_avg_ms', 'w_avg_ms', 'r_p99_ms', 'w_p99_ms'])
        for _, lab, r in data:
            for p in POL:
                w.writerow([lab.replace('\n', ' '), p] + [round(r[p][m], 4) for m in ('tput', 'fr', 'waf', 'r_avg', 'w_avg', 'r_p99', 'w_p99')])

    # compact figure* panels: rows share the x axis, one legend, group names on the top row, workloads on the bottom row
    def rows_fig(rows, name, h):
        fig, axs = plt.subplots(len(rows), 1, figsize=(7.0, h), sharex=True, squeeze=False)
        axs = axs[:, 0]
        for k, (ax, (m, yl, kw)) in enumerate(zip(axs, rows)):
            yt = kw.pop('yticks', None)
            xs = panel(ax, data, m, yl, '', xt=(k == len(rows) - 1), **kw)
            ax.set_ylabel(yl, fontsize=5.4, labelpad=1.5, linespacing=0.9)
            if yt:
                ax.set_yticks(yt)
                if kw.get('logy'):
                    ax.set_yticklabels([f'{t:g}' for t in yt]); ax.minorticks_off()
        groups(axs[0], data, xs)
        fig.align_ylabels(axs)
        fig.tight_layout(pad=0.15, h_pad=0.25); fig.subplots_adjust(top=1 - 0.27 / h); legend(fig, 1.005)
        save(fig, name)

    rows_fig([('fr', 'Reads/read', dict(clip=2.2, ylim=(0, 2.45), fmt='{:.2f}', yticks=[0, 1, 2])),
              ('r_avg', 'Avg. read\n(ms)', dict(logy=True, ylim=(0.05, 50), yticks=[0.1, 1, 10])),
              ('r_p99', 'p99 read\n(ms)', dict(logy=True, ylim=(0.1, 500), yticks=[1, 10, 100]))],
             'fig_eval_read', 2.04)
    rows_fig([('waf', 'WA', dict(logy=True, ylim=(1, 100))),
              ('w_avg', 'Avg. write\n(ms)', dict(logy=True, ylim=(0.05, 50), yticks=[0.1, 1, 10])),
              ('w_p99', 'p99 write\n(ms)', dict(logy=True, ylim=(0.2, 1000), yticks=[1, 10, 100, 1000]))],
             'fig_eval_write', 2.04)
    rows_fig([('tput', 'Throughput', dict(norm=True, clip=1.3, ylim=(0, 1.42), fmt='{:.2f}',
                                                    yticks=[0, 0.5, 1]))], 'fig_eval_tput', 0.96)

    gm = lambda v: math.exp(sum(map(math.log, v)) / len(v))
    for m in ('tput', 'r_avg', 'w_avg', 'r_p99', 'w_p99', 'waf', 'fr'):
        print(m, {LABEL[p]: round(gm([r[p][m] / r['page4k'][m] for _, l, r in data
                                      if not (m in WRITE_M and l in READ_ONLY)]), 3) for p in POL[1:]})
    print('wrote', OUT + '/fig_eval_{read,write,tput}.{pdf,png} and', PAPER)


if __name__ == '__main__':
    main()
