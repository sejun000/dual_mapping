#!/usr/bin/env python3
"""Proposed + spill=2 (results/final_rp_spill) next to proposed without spill (results/final_rp) on the 11 paper workloads.

Metrics as tools/plot_rp.py (apps: block-layer latency, traces: replayer completion latency, fio: fio clat; ms) plus
device map reads per host read page (= read_map / host_read_pages over the measured phase, as tools/apps/summarize.py)
and map programs per host write page.
  python3 tools/cmp_spill.py            -> table on stdout (+ results/final_rp_spill/COMPARE.txt)
"""
import json, math, os, re, sys

BASE = '/home/sejun000/dual_mapping/results/final_rp'
SPILL = '/home/sejun000/dual_mapping/results/final_rp_spill'
NAN = float('nan')
WL = [('trace', 'ali3', 'Ali-low'), ('trace', 'spfresh', 'SPFresh'),
      ('app', 'ycsba_d_t64', 'YCSB-A'), ('app', 'ycsbb_d_t64', 'YCSB-B'), ('app', 'ycsbc_d_t64', 'YCSB-C'),
      ('app', 'ycsbe_d_t64', 'YCSB-E'), ('app', 'tpcc_d_t64', 'TPC-C'),
      ('fio', (50, 70), 'R50 4K-hv'), ('fio', (50, 30), 'R50 128K-hv'), ('fio', (90, 70), 'R90 4K-hv'), ('fio', (90, 30), 'R90 128K-hv')]


def fnum(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return NAN


def dm_line(f):
    s = open(f).read()
    m = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', s)}
    r = re.search(r'reads\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+)\)', s)
    g = re.search(r'\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+) other (\d+)\)', s)
    if r: m['rmap'] = int(r.group(5))
    if g: m['pmap'] = int(g.group(5))
    return m


def mapstats(a, b):
    try:
        a, b = dm_line(a), dm_line(b)
        hw = b['submitted'] - a['submitted']; hr = b['host_read_pages'] - a['host_read_pages']
        return (b['rmap'] - a['rmap']) / hr if hr else NAN, (b['pmap'] - a['pmap']) / hw if hw else NAN
    except Exception:
        return NAN, NAN


def app_row(R, wl, alias):
    s = dict(l.strip().split('=', 1) for l in open(f'{R}/apps/{wl}/{alias}/summary.txt') if '=' in l)
    if s.get('status') != 'DONE':
        raise ValueError('status ' + str(s.get('status')))
    g = lambda k: fnum(s.get(k))
    mp = g('dev_waf_map')
    return {'tput': g('throughput'), 'waf': g('dev_waf'), 'rd_avg': g('blk_read_lat_us_mean') / 1e3,
            'rd_p99': g('blk_read_lat_us_p99') / 1e3, 'wr_avg': g('blk_write_lat_us_mean') / 1e3,
            'mapR_rd': g('dev_map_reads_per_host_read'), 'mapP_wr': mp}


def trace_row(R, t, pol):
    line = next(l for l in open(f'{R}/trace_{t}/SUMMARY.txt') if l.startswith(pol + ' '))
    j = json.load(open(f'{R}/trace_{t}/{pol}/summary.json'))
    r, w = j['read_lat_us'], j['write_lat_us']
    o = f'{R}/trace_{t}/{pol}'
    mr, mp = mapstats(o + '/stats_warm_end.txt', o + '/stats_end.txt')
    return {'tput': float(re.search(r'IOPS=\s*(\d+)', line).group(1)), 'waf': float(re.search(r'WAF=\s*([\d.]+)', line).group(1)),
            'rd_avg': r['mean'] / 1e3, 'rd_p99': r['p99'] / 1e3, 'wr_avg': w['mean'] / 1e3, 'mapR_rd': mr, 'mapP_wr': mp}


def fio_row(R, cfg, pol):
    rd, s = cfg
    d = f'{R}/fio/{pol}_r{rd}_s{s}'
    t = open(d + '/summary.txt').read()
    j = json.load(open(d + '/mixbs.json'))['jobs'][0]
    c = lambda k: j[k]['clat_ns']
    mr, mp = mapstats(d + '/stats_before.txt', d + '/stats_after.txt')
    return {'tput': float(re.search(r'total IOPS=(\d+)', t).group(1)), 'waf': float(re.search(r'WAF=([\d.]+)', t).group(1)),
            'rd_avg': c('read')['mean'] / 1e6, 'rd_p99': c('read')['percentile']['99.000000'] / 1e6,
            'wr_avg': c('write')['mean'] / 1e6, 'mapR_rd': mr, 'mapP_wr': mp}


def row(R, kind, key, spill):
    try:
        if kind == 'app':
            return app_row(R, key, 'proposed_b12s' if spill else 'proposed_b12f')
        return (trace_row if kind == 'trace' else fio_row)(R, key, 'proposed')
    except Exception as e:
        return {'err': str(e)[:40]}


def fmt(v, w=9, p=3):
    return f'{v:{w}.{p}f}' if isinstance(v, float) and not math.isnan(v) else f'{"-":>{w}}'


def main():
    out = ['proposed (no spill, results/final_rp) vs proposed + spill=2 (results/final_rp_spill); 12.5% mapping DRAM',
           'tput = ops/s (apps) or IOPS (traces, fio); latency ms; mapR/rd = flash map reads per host read page; '
           'mapP/wr = map programs per host write page (apps: dev_waf_map)',
           f'{"workload":12s} {"variant":7s} {"tput":>10s} {"WAF":>7s} {"rd_avg":>8s} {"rd_p99":>8s} {"wr_avg":>8s} {"mapR/rd":>8s} {"mapP/wr":>8s}  d_tput  d_WAF']
    for kind, key, lab in WL:
        a, b = row(BASE, kind, key, False), row(SPILL, kind, key, True)
        for tag, r in (('base', a), ('spill2', b)):
            if 'err' in r:
                out.append(f'{lab:12s} {tag:7s} (missing: {r["err"]})'); continue
            extra = ''
            if tag == 'spill2' and 'err' not in a:
                extra = f'  {100 * (r["tput"] / a["tput"] - 1):+5.1f}% {100 * (r["waf"] / a["waf"] - 1):+5.1f}%'
            out.append(f'{lab:12s} {tag:7s} {fmt(r["tput"], 10, 0)} {fmt(r["waf"], 7, 3)} {fmt(r["rd_avg"], 8, 3)} {fmt(r["rd_p99"], 8, 2)} '
                       f'{fmt(r["wr_avg"], 8, 3)} {fmt(r["mapR_rd"], 8, 4)} {fmt(r["mapP_wr"], 8, 4)}{extra}')
    txt = '\n'.join(out)
    print(txt)
    if os.path.isdir(SPILL):
        open(SPILL + '/COMPARE.txt', 'w').write(txt + '\n')


if __name__ == '__main__':
    main()
