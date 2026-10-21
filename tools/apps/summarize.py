#!/usr/bin/env python3
"""Summaries for tools/apps runs.

  summarize.py case  <case_dir>      -> key=value lines on stdout, <case_dir>/summary.json
  summarize.py table [results_root]  -> one TSV row per case (default results/apps)

Metric definitions follow splitEval scripts/plot.py (cell_metrics / app_latency) where they exist:
  dev_*_lat_ms_avg : /sys/block stat ticks / ios over the measurement window (plot.py write_lat_ms/read_lat_ms)
  write_qd_max     : max of the 1 s /sys/block inflight write samples
  app latency      : filebench flowop avg/max (study FLOWOPS table), YCSB per-op avg + percentiles (mean over
                     tenants, as the study), TPC-C merged LatencySuccessHistogramMs with in-bucket interpolation,
                     fio clat percentiles
New here (device counters of the dm_ftl cores, NVMe-flush snapshots):
  dev_waf = NAND programs / host-submitted 4 KiB pages; map reads per host page; GC / RMW / fold / map shares.
  fs_waf  = bytes written to the block device / bytes applications wrote through ext4's buffered path (bpftrace
            ext4_da_write_begin + ext4_write_begin tracepoints). fio: measured phase only (after its O_DIRECT prefill).
  blk_*_p*: block-layer issue->complete latency percentiles (bpftrace, 2 us resolution below 1 ms).
"""
import glob, json, math, os, re, statistics, sys

FLOWOPS = {   # plot.py FLOWOPS: (write-path flowops, read-path flowops)
    "varmail-hot1g": (("fsyncfile2",), ("readfile3",)),
    "oltp": (("dbwr-block",), ("shadowread",)),
    "fileserverfs": (("fsyncfile1", "fsyncappend1"), ("readfile1",)),
}
TPCC_READ = ("OrderStatus", "StockLevel")
TPCC_WRITE = ("NewOrder", "Payment", "Delivery")


def kv(path):
    out = {}
    try:
        for line in open(path):
            if '=' in line:
                k, v = line.rstrip('\n').split('=', 1)
                out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def read(path):
    try:
        return open(path, errors='replace').read()
    except OSError:
        return ''


# ------------------------------------------------------------------ device counters
def dm_counters(path):
    s = read(path)
    if '[dm ' not in s:
        return None
    m = {k: int(v) for k, v in re.findall(r'\b(\w+)=(\d+)', s)}
    p = re.search(r'prog=\d+ \(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+) other (\d+)\)', s)
    r = re.search(r'reads\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+)\)', s)
    if p:
        for k, v in zip(('host', 'gc', 'rmw', 'fold', 'map', 'other'), p.groups()):
            m['prog_' + k] = int(v)
    if r:
        for k, v in zip(('host', 'gc', 'rmw', 'fold', 'map'), r.groups()):
            m['read_' + k] = int(v)
    return m


def dm_delta(a, b, prefix, out):
    if not a or not b:
        return
    d = {k: b[k] - a[k] for k in b if k in a and k not in ('L', 'NB', 'P', 'mem', 'WAFx1000')}
    hw, hr = d.get('submitted', 0), d.get('host_read_pages', 0)
    out[prefix + 'host_write_pages'] = hw
    out[prefix + 'host_read_pages'] = hr
    out[prefix + 'nand_progs'] = d.get('prog', 0)
    out[prefix + 'waf'] = d.get('prog', 0) / hw if hw else math.nan
    for k in ('gc', 'rmw', 'fold', 'map', 'other'):
        out[prefix + 'waf_' + k] = d.get('prog_' + k, 0) / hw if hw else math.nan
    nread = sum(d.get('read_' + k, 0) for k in ('host', 'gc', 'rmw', 'fold', 'map'))
    out[prefix + 'nand_reads'] = nread
    out[prefix + 'map_reads_per_host_page'] = d.get('read_map', 0) / (hw + hr) if hw + hr else math.nan
    out[prefix + 'map_reads_per_host_read'] = d.get('read_map', 0) / hr if hr else math.nan
    out[prefix + 'erases'] = d.get('erases', 0)
    out[prefix + 'map_miss'] = d.get('map_miss', 0)
    out[prefix + 'mapping_mem_bytes_end'] = b.get('mem', 0)


# ------------------------------------------------------------------ block layer
def stat(path):
    try:
        return [int(x) for x in open(path).read().split()]
    except (OSError, ValueError):
        return None


def blk_delta(a, b, out, prefix='blk_'):
    if not a or not b:
        return
    d = [y - x for x, y in zip(a, b)]
    rios, wios = d[0], d[4]
    out[prefix + 'read_ios'] = rios
    out[prefix + 'write_ios'] = wios
    out[prefix + 'read_bytes'] = d[2] * 512
    out[prefix + 'write_bytes'] = d[6] * 512
    out[prefix + 'avg_write_req_kb'] = d[6] * 512 / wios / 1024 if wios else math.nan
    out['dev_read_lat_ms_avg'] = d[3] / rios if rios else math.nan
    out['dev_write_lat_ms_avg'] = d[7] / wios if wios else math.nan


def qd_max(path):
    vals = []
    for line in read(path).splitlines():
        p = line.split()
        if len(p) > 2 and p[0][:1].isdigit():
            vals.append(int(p[2]))
    return max(vals) if vals else math.nan


def bucket_lo_hi(b):
    if b < 500:
        return 2 * b, 2 * b + 2
    if b < 950:
        lo = 1000 + 20 * (b - 500); return lo, lo + 20
    if b < 1940:
        lo = 10000 + 1000 * (b - 950); return lo, lo + 1000
    if b < 2930:
        lo = 1000000 + 100000 * (b - 1940); return lo, lo + 100000
    return 100000000, 100000000


def hist_pct(h, p):
    """h: {bucket: count}; linear interpolation inside the bucket (us)."""
    n = sum(h.values())
    if not n:
        return math.nan
    t, c = p * n, 0
    for b in sorted(h):
        x = h[b]
        if c + x >= t:
            lo, hi = bucket_lo_hi(b)
            return lo + (t - c) / x * (hi - lo)
        c += x
    return bucket_lo_hi(max(h))[1]


def blklat(path, out):
    s = read(path)
    if not s:
        return
    hists = {0: {}, 1: {}}
    for c, b, n in re.findall(r'^@lat\[(\d+), (\d+)\]: (\d+)', s, re.M):
        if int(c) in hists:
            hists[int(c)][int(b)] = int(n)
    sums = {int(c): int(v) for c, v in re.findall(r'^@lat_sum_us\[(\d+)\]: (\d+)', s, re.M)}
    cnts = {int(c): int(v) for c, v in re.findall(r'^@lat_n\[(\d+)\]: (\d+)', s, re.M)}
    for c, name in ((0, 'read'), (1, 'write')):
        if cnts.get(c):
            out[f'blk_{name}_lat_us_mean'] = sums.get(c, 0) / cnts[c]
        for p in (50, 90, 99, 99.9, 99.99):
            out[f'blk_{name}_lat_us_p{p:g}'] = hist_pct(hists[c], p / 100)
    m = re.search(r'^@app_write_bytes: (\d+)', s, re.M)
    out['app_write_bytes'] = int(m.group(1)) if m else 0


def df_util(path):
    """df Use% as used / (used + avail), from a saved `df -B1` output."""
    try:
        f = read(path).splitlines()[1].split()
        used, avail = int(f[2]), int(f[3])
        return 100.0 * used / (used + avail)
    except (IndexError, ValueError):
        return None


# ------------------------------------------------------------------ workloads
_FLOWOP_RE = re.compile(r"^(\S+)\s+(\d+)ops\s+\S+ops/s\s+\S+mb/s\s+([\d.]+)ms/op\s+\[([\d.]+)ms - ([\d.]+)ms\]")


def filebench(case, cfg, out):
    log = read(os.path.join(case, 'filebench_run.log'))
    m = re.search(r'IO Summary: (\d+) ops ([\d.]+) ops/s ([\d]+)/([\d]+) rd/wr\s+([\d.]+)mb/s ([\d.]+)ms/op', log)
    if m:
        out['throughput'] = float(m.group(2)); out['throughput_unit'] = 'ops/s'
        out['fb_ops'] = int(m.group(1)); out['fb_mbps'] = float(m.group(5)); out['fb_ms_per_op'] = float(m.group(6))
    seen = {}
    for line in log.splitlines():
        mm = _FLOWOP_RE.match(line.strip())
        if mm:
            seen[mm.group(1)] = (int(mm.group(2)), float(mm.group(3)), float(mm.group(5)))
    out['fb_flowops'] = {k: {'ops': v[0], 'avg_ms': v[1], 'max_ms': v[2]} for k, v in seen.items()}
    prof = cfg.get('filebench_profile', '')
    if prof in FLOWOPS:
        for side, names in zip(('write', 'read'), FLOWOPS[prof]):
            rows = [seen[n] for n in names if n in seen]
            tot = sum(r[0] for r in rows)
            out[f'app_{side}_lat_ms_avg'] = sum(r[0] * r[1] for r in rows) / tot if tot else math.nan
            out[f'app_{side}_lat_ms_max'] = max((r[2] for r in rows), default=math.nan)


def fio(case, out):
    try:
        j = json.load(open(os.path.join(case, 'fio.json')))['jobs'][0]
    except Exception:
        return
    w, r = j['write'], j['read']
    out['throughput'] = w['iops'] + r['iops']; out['throughput_unit'] = 'IOPS'
    for name, x in (('write', w), ('read', r)):
        if x['iops'] <= 0:
            continue
        out[f'fio_{name}_iops'] = x['iops']
        out[f'fio_{name}_MBps'] = x['bw'] / 1024
        c = x['clat_ns']
        out[f'app_{name}_lat_ms_avg'] = c['mean'] / 1e6
        for k, v in (c.get('percentile') or {}).items():
            out[f'fio_{name}_clat_us_p{float(k):g}'] = v / 1e3
        out[f'app_{name}_lat_ms_p99'] = (c.get('percentile') or {}).get('99.000000', math.nan) / 1e6
        out[f'app_{name}_lat_ms_max'] = c['max'] / 1e6
    if 'sync' in j and j['sync'].get('lat_ns', {}).get('total_ios'):
        s = j['sync']['lat_ns']
        out['fio_fdatasync_us_mean'] = s['mean'] / 1e3
        for k, v in (s.get('percentile') or {}).items():
            out[f'fio_fdatasync_us_p{float(k):g}'] = v / 1e3


def ycsb(case, cfg, out):
    acc, thr = {}, []
    pat = re.compile(r'^\[(READ|UPDATE|READ-MODIFY-WRITE|INSERT|SCAN)\], (Operations|AverageLatency\(us\)|'
                     r'([\d.]+)(?:th)?PercentileLatency\(us\)|MaxLatency\(us\)), ([\d.]+)')
    for f in sorted(glob.glob(os.path.join(case, 'ycsb_run_*.log'))):
        for line in open(f, errors='replace'):
            m = pat.match(line)
            if m:
                key = m.group(2) if not m.group(3) else 'p' + m.group(3)
                acc.setdefault(m.group(1), {}).setdefault(key, []).append(float(m.group(4)))
            m = re.match(r'\[OVERALL\], Throughput\(ops/sec\), ([\d.]+)', line)
            if m:
                thr.append(float(m.group(1)))
    if thr:
        out['throughput'] = sum(thr); out['throughput_unit'] = 'ops/s'; out['ycsb_tenants'] = len(thr)
    ops = {}
    for op, d in acc.items():
        n, a = d.get('Operations', []), d.get('AverageLatency(us)', [])
        e = {'ops': sum(n), 'avg_us': sum(x * y for x, y in zip(n, a)) / sum(n) if n and sum(n) else math.nan}
        for k, v in d.items():
            if k.startswith('p'):
                e[k + '_us'] = statistics.mean(v)
        if d.get('MaxLatency(us)'):
            e['max_us'] = max(d['MaxLatency(us)'])
        ops[op] = e
    out['ycsb_ops'] = ops
    wop = 'READ-MODIFY-WRITE' if cfg.get('ycsb_workload') == 'workloadf' else 'UPDATE'
    if wop not in ops and 'INSERT' in ops: wop = 'INSERT'          # workloads D/E
    rop = 'READ' if 'READ' in ops else 'SCAN'                      # workload E
    for side, op in (('write', wop), ('read', rop)):
        if op in ops:
            out[f'app_{side}_lat_ms_avg'] = ops[op]['avg_us'] / 1e3
            out[f'app_{side}_lat_ms_p99'] = ops[op].get('p99_us', math.nan) / 1e3


def tpcc_hist(case, txns=None):
    bl, cnt = None, None
    for f in glob.glob(os.path.join(case, 'bb_*', '*.raw.json')):
        for t, v in json.load(open(f)).items():
            h = v.get('LatencySuccessHistogramMs') if isinstance(v, dict) else None
            if not h or t == 'Invalid' or (txns and t not in txns):
                continue
            if bl is None:
                bl, cnt = h['bucketlist'], [0] * len(h['buckets'])
            for i, c in enumerate(h['buckets']):
                cnt[i] += c
    if not cnt or not sum(cnt):
        return None
    n = sum(cnt); lo = [0] + bl[:-1]
    avg = sum(((l + u) / 2) * c for l, u, c in zip(lo, bl, cnt)) / n

    def q(p):
        t, c = p * n, 0
        for i, x in enumerate(cnt):
            if c + x >= t:
                return lo[i] + ((t - c) / x if x else 0) * (bl[i] - lo[i])
            c += x
        return bl[-1]
    return {'avg_ms': avg, 'p50_ms': q(.5), 'p90_ms': q(.9), 'p95_ms': q(.95), 'p99_ms': q(.99), 'n': n}


def tpcc(case, out):
    tot = 0.0; n = 0
    for f in glob.glob(os.path.join(case, 'bb_*', '*.summary.json')):
        tot += json.load(open(f))['Throughput (requests/second)']; n += 1
    if n:
        out['throughput'] = tot; out['throughput_unit'] = 'txn/s'; out['tpcc_tenants'] = n
    for side, txns in (('write', TPCC_WRITE), ('read', TPCC_READ), ('all', None)):
        h = tpcc_hist(case, txns)
        if h:
            out[f'tpcc_{side}'] = h
            if side != 'all':
                out[f'app_{side}_lat_ms_avg'] = h['avg_ms']; out[f'app_{side}_lat_ms_p99'] = h['p99_ms']


# ------------------------------------------------------------------ case
def case_metrics(case):
    cfg = kv(os.path.join(case, 'experiment_config.txt'))
    out = {'case': case, 'workload': cfg.get('workload'), 'policy': cfg.get('policy'),
           'status': read(os.path.join(case, 'STATUS')).strip()}
    st = lambda n: os.path.join(case, n)
    out['start_util_pct'] = df_util(st('start_df.txt'))
    out['end_util_pct'] = df_util(st('end_df.txt'))
    wc = kv(st('workload_config.txt'))
    if 'measure_util_pct' in wc:
        out['measure_util_pct'] = float(wc['measure_util_pct'])
    a, b = kv(st('start_ext4_sysfs.txt')), kv(st('end_ext4_sysfs.txt'))
    if a.get('session_write_kbytes', '').isdigit() and b.get('session_write_kbytes', '').isdigit():
        out['ext4_write_kbytes_delta'] = int(b['session_write_kbytes']) - int(a['session_write_kbytes'])
    blk_delta(stat(st('start_block_stat.txt')), stat(st('end_block_stat.txt')), out)
    out['write_qd_max'] = qd_max(st('data_device_timeseries.txt'))
    blklat(st('blklat.txt'), out)
    if out.get('app_write_bytes'):
        out['fs_waf'] = out.get('blk_write_bytes', 0) / out['app_write_bytes']
    a, b = dm_counters(st('dm_start.txt')), dm_counters(st('dm_end.txt'))
    dm_delta(a, b, 'dev_', out)
    if os.path.exists(st('dm_measure.txt')):   # fio: measured phase only (after the prefill)
        dm_delta(dm_counters(st('dm_measure.txt')), b, 'devmeas_', out)
        tmp = {}
        blk_delta(stat(st('measure_block_stat.txt')), stat(st('end_block_stat.txt')), tmp, 'blkmeas_')
        out.update({k: v for k, v in tmp.items() if k.startswith('blkmeas_')})
        if out.get('app_write_bytes'):
            out['fs_waf'] = out.get('blkmeas_write_bytes', 0) / out['app_write_bytes']
    dw = out.get('devmeas_waf', out.get('dev_waf'))   # fio: measured phase on both factors
    if dw is not None and 'fs_waf' in out:
        out['total_waf'] = dw * out['fs_waf']
    mc = kv(st('memcg_end.txt'))
    for k in ('memory.max', 'memory.high', 'memory.peak', 'memory.events.high', 'memory.events.max',
              'memory.events.oom_kill', 'memory.swap.current'):
        if mc.get(k, '').isdigit():
            out['memcg_' + k.replace('memory.', '').replace('.', '_')] = int(mc[k])
    kind = cfg.get('wl_kind', '')
    if kind.startswith('filebench'):
        filebench(case, cfg, out)
    elif kind.startswith('fio'):
        fio(case, out)
    elif kind == 'ycsb-multi':
        ycsb(case, cfg, out)
    elif kind == 'tpcc-multi':
        tpcc(case, out)
    return out


def flat(d, prefix=''):
    for k, v in d.items():
        if isinstance(v, dict):
            yield from flat(v, prefix + k + '.')
        else:
            yield prefix + k, v


def fmt(v):
    if isinstance(v, float):
        return 'nan' if math.isnan(v) else f'{v:.6g}'
    return str(v)


TABLE_COLS = ['workload', 'policy', 'status', 'throughput', 'throughput_unit', 'start_util_pct', 'measure_util_pct',
              'app_write_lat_ms_avg', 'app_write_lat_ms_p99', 'app_read_lat_ms_avg', 'app_read_lat_ms_p99',
              'blk_write_lat_us_p50', 'blk_write_lat_us_p99', 'blk_read_lat_us_p50', 'blk_read_lat_us_p99',
              'dev_write_lat_ms_avg', 'dev_read_lat_ms_avg', 'write_qd_max',
              'dev_waf', 'devmeas_waf', 'fs_waf', 'total_waf', 'dev_waf_gc', 'dev_waf_rmw', 'dev_waf_fold', 'dev_waf_map',
              'dev_map_reads_per_host_page', 'dev_map_reads_per_host_read', 'dev_erases', 'dev_mapping_mem_bytes_end',
              'memcg_max', 'memcg_peak', 'memcg_events_high', 'memcg_events_max', 'memcg_events_oom_kill']


def main():
    if len(sys.argv) >= 3 and sys.argv[1] == 'case':
        case = sys.argv[2].rstrip('/')
        m = case_metrics(case)
        json.dump(m, open(os.path.join(case, 'summary.json'), 'w'), indent=1, default=str)
        for k, v in flat(m):
            print(f'{k}={fmt(v)}')
    elif len(sys.argv) >= 2 and sys.argv[1] == 'table':
        root = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'results', 'apps')
        print('\t'.join(TABLE_COLS))
        for st in sorted(glob.glob(os.path.join(root, '*', '*', 'STATUS')) + glob.glob(os.path.join(root, '*', 'STATUS'))):
            case = os.path.dirname(st)
            if '.old.' in case:
                continue
            m = case_metrics(case)
            print('\t'.join(fmt(m.get(c, '')) for c in TABLE_COLS))
    else:
        print(__doc__); sys.exit(2)


if __name__ == '__main__':
    main()
