#!/usr/bin/env python3
"""Simulator sensitivity study (DAC'27 Part A): IU size, mapping-DRAM budget, OP, write-buffer size.

  tools/sens.py jobs  [study ...]   -> job lines "<outdir> <tag> <ftlsim args...>" (deduplicated across studies)
  tools/sens.py run   [study ...]   -> run the missing jobs (xargs, pinned to NUMA node 0 even CPUs 0-92)
  tools/sens.py table               -> results/sens/SUMMARY.txt (WAF tables + trends)

Fixed baseline (everything not swept): OP 7%, mapping DRAM 18.75% of the 4 B page map, 1 MiB write buffer,
proposed = V3 (U=16, fine_quota 0.0429 carved from OP, cuckoo2 table sized by map_budget - coarse map).
Workloads: the results/op7b set (full traces, same arguments as tools/run_op7_fix.sh) + the six captured
application traces in the aged condition (identity addresses, full sequential fill first, NVMeVirt 44G geometry,
measurement window = capture markers, as tools/apps_trace_sim.sh).
Geometry: P is pinned to the U=16 auto value for every U (it is a multiple of 1024 pages), so U changes nothing
else. U=32 on the 44G app device: L is rounded down to a multiple of 32 (16 pages less, as the NVMeVirt glue does),
op is raised by 3e-6 so NB stays 373, and the 16 trace pages beyond L are dropped (trace_clip=1).
OP sweep: the fine pool stays carved from OP with the same fraction rule fine_quota = 0.0429 * OP / 0.07
(= 61.3% of the OP fraction, as a fraction of the physical blocks); variant "fixq" keeps fine_quota = 0.0429.
"""
import json, os, subprocess, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'results', 'sens')
SIM = os.environ.get('SIM', os.path.join(ROOT, 'ftlsim', 'ftlsim'))
CPUS = os.environ.get('CPUS', '0-92:2')

SYN = {'z0': 'zipf=0', 'z0.99': 'zipf=0.99', 'z1.2': 'zipf=1.2'}
TR = {
    'ali1': ('/home/sejun000/traces_dm/ali1.csv', 276605960192),
    'ali2': ('/home/sejun000/traces_dm/ali2.csv', 368702390272),
    'ali3': ('/home/sejun000/traces_dm/ali3.csv', 2587949531136),
    'ssd': ('/home/sejun000/traces_dm/ssd.csv', 423542915072),
    'kv_w1': ('/mnt/nvme_mounted/kvtrace/w1.packed.csv', 67001909248),
    'spfresh': ('/mnt/nvme_mounted/spfresh_trace/spf.packed.csv', 42970644480),
}
APPS = ['ycsbb_d_t64', 'ycsba_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'fio_randwrite_d', 'fileserverfs']
APPROOT = '/mnt/nvme_mounted/dm_apptrace'
WLS = list(SYN) + list(TR) + APPS
SIMWL = list(SYN) + list(TR)

V3 = ('mode=proposed fine_extra=0 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 '
      'adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1')
BUDGETS = [625, 1000, 1250, 1875, 2500, 3750]   # basis points of the page map (L x 4 B)
OPS = [0.07, 0.12, 0.20]
WBUFS = ['256K', '1M', '4M']


def auto_p(cap, U=16):
    pages = cap // 4096
    p = pages * 3 // 1024
    p -= p % (64 * U)
    return max(p, 64 * U)


def app_geo(wl):
    j = json.load(open(f'{APPROOT}/{wl}/trace.json'))
    m = {x['name']: x for x in j.get('markers', [])}
    warm = m.get('dm_measure_prefill_done', m.get('dm_measure_start'))
    stop = m.get('dm_measure_end')
    return j['capacity_bytes'], warm['write_pages_before'], stop['write_pages_before']


def base(wl, U, op):
    """workload + geometry arguments; returns (args, L pages)"""
    if wl in SYN:
        cap = 64 << 30
        return f'{SYN[wl]} P={auto_p(cap)}', cap // 4096 // U * U
    if wl in TR:
        f, cap = TR[wl]
        return f'trace={f} capacity={cap} P={auto_p(cap)}', cap // 4096 // U * U
    cap, warm, stop = app_geo(wl)
    L = cap // 4096
    assert L == 10798160, wl
    a = f'trace={APPROOT}/{wl}/trace.csv trace_remap=0 seq_fill=1 capacity={cap} P=30976 trace_warmup_pages={warm} trace_stop_pages={stop}'
    Lu = L - L % U
    if abs(op - 0.07) < 1e-9:
        # NVMeVirt 44G: 373 lines x 30976 pages; keep NB = 373 when L shrinks to a multiple of U
        op = 373 * 30976 / Lu - 1 + 1e-9
    if Lu != L:
        a += ' trace_clip=1'
    return a, Lu, op


def wl_args(wl, U, op):
    r = base(wl, U, op)
    if len(r) == 3:
        a, L, op_eff = r
    else:
        (a, L), op_eff = r, op
    return a, L, op_eff


def cfg(pol, U=16, b=1875, op=0.07, wb='1M', fixq=False):
    """tag and policy args (budget b in basis points of the page map); workload-independent part"""
    tag = {'prop': f'prop_U{U}_b{b}', 'iu_rmw': f'iurmw_U{U}', 'page4k': 'page4k',
           'dftl': f'dftl_b{b}', 'leaftl': f'leaftl_b{b}', 'mu': f'mu_b{b}'}[pol]
    if pol == 'prop' and fixq:
        tag += '_fixq'
    if abs(op - 0.07) > 1e-9:
        tag += f'_op{int(round(op * 100))}'
    if wb != '1M':
        tag += f'_wb{wb}'
    return tag


def job(wl, pol, U=16, b=1875, op=0.07, wb='1M', fixq=False):
    a, L, op_eff = wl_args(wl, U if pol in ('prop', 'iu_rmw') else 16, op)
    tag = cfg(pol, U, b, op, wb, fixq)
    by = L * 4 * b // 10000
    if pol == 'prop':
        fq = 0.0429 if fixq else 0.0429 * op / 0.07
        p = f'{V3} U={U} fine_quota={fq:.6g} map_budget={b / 10000:g}'
    elif pol == 'iu_rmw':
        p = f'mode=iu_rmw U={U}'
    elif pol == 'page4k':
        p = 'mode=page4k'
    elif pol == 'dftl':
        p = f'mode=dftl cache_bytes={by} dftl_tpbuf=1'
    elif pol == 'leaftl':
        p = f'mode=leaftl cache_bytes={by}'
    elif pol == 'mu':
        p = f'mode=mu cache_bytes={by}'
    args = f'{a} op={op_eff:.12g} {p}'
    if wb != '1M':
        args += f' wbuf={wb}'
    return wl, tag, args


def studies():
    S = {}
    # A1: IU size at the 18.75% budget, proposed and IU+RMW
    S['A1'] = [job(w, 'prop', U=u) for w in WLS for u in (8, 16, 32)] + \
              [job(w, 'iu_rmw', U=u) for w in WLS for u in (8, 16, 32)]
    # A2: DRAM budget; proposed on the feasible U x budget grid (coarse map 4/U of the page map must leave room)
    a2 = []
    for w in WLS:
        for b in BUDGETS:
            for u in (8, 16, 32):
                if 10000 / u < b - 1e-9 and b - 10000 / u >= 300:   # >= ~3% of the page map left for the table
                    a2.append(job(w, 'prop', U=u, b=b))
            for pol in ('dftl', 'leaftl', 'mu'):
                a2.append(job(w, pol, b=b))
        a2.append(job(w, 'page4k'))
    S['A2'] = a2
    # A3: OP
    S['A3'] = [job(w, pol, op=op) for w in WLS for op in OPS for pol in ('prop', 'page4k', 'dftl', 'leaftl')] + \
              [job(w, 'prop', op=op, fixq=True) for w in WLS for op in OPS if op != 0.07]
    # A4: write buffer
    S['A4'] = [job(w, pol, wb=wb) for w in WLS for wb in WBUFS for pol in ('prop', 'page4k', 'dftl', 'leaftl', 'iu_rmw')]
    return S


def all_jobs(sel):
    S = studies(); seen = {}; out = []
    for k in sel or S:
        for wl, tag, args in S[k]:
            key = (wl, tag)
            if key in seen:
                assert seen[key] == args, (key, seen[key], args)
                continue
            seen[key] = args; out.append((wl, tag, args))
    return out


def est(wl, tag):   # rough runtime weight for ordering (longest first)
    w = {'ali3': 40, 'ali2': 8, 'ali1': 5, 'spfresh': 4, 'ssd': 3}.get(wl, 2)
    t = 6 if tag.startswith('mu') else 3 if tag.startswith(('dftl', 'leaftl')) else 1
    return w * t


def run(sel):
    todo = [j for j in all_jobs(sel) if not os.path.exists(f'{OUT}/{j[0]}/{j[1]}.json') or
            os.path.getsize(f'{OUT}/{j[0]}/{j[1]}.json') == 0 or not done_ok(f'{OUT}/{j[0]}/{j[1]}.json')]
    todo.sort(key=lambda j: -est(j[0], j[1]))
    heavy = [j for j in todo if j[0] == 'ali3']; light = [j for j in todo if j[0] != 'ali3']
    print(f'{len(todo)} jobs ({len(heavy)} ali3)', flush=True)
    procs = []
    for name, js, par in (('heavy', heavy, int(os.environ.get('PH', 8))), ('light', light, int(os.environ.get('PL', 26)))):
        lst = f'{OUT}/jobs_{name}.txt'
        os.makedirs(OUT, exist_ok=True)
        with open(lst, 'w') as f:
            for wl, tag, args in js:
                f.write(f'{OUT}/{wl} {tag} {args}\n')
        cmd = (f"xargs -P {par} -L 1 bash -c 'o=$0; t=$1; shift 1; mkdir -p $o; "
               f"/usr/bin/time -f \"%e s %M KB\" -o $o/$t.time taskset -c {CPUS} {SIM} \"$@\" tag=$t > $o/$t.json 2> $o/$t.log' < {lst}")
        procs.append(subprocess.Popen(cmd, shell=True))
    for p in procs:
        p.wait()


def done_ok(f):
    try:
        json.load(open(f)); return True
    except Exception:
        return False


def W(wl, tag):
    try:
        return json.load(open(f'{OUT}/{wl}/{tag}.json'))['WAF']
    except Exception:
        return None


def fmt(x, w=7):
    return f'{x:{w}.3f}' if isinstance(x, float) else f'{"-":>{w}s}'


def table():
    L = []
    P = lambda *a: L.append(' '.join(str(x) for x in a))
    P('# Sensitivity study (tools/sens.py). WAF = total programs / submitted host pages (measurement window).')
    P('# Baseline: OP 7%, DRAM 18.75% of page map, wbuf 1 MiB, V3 proposed U=16. Apps = captured traces, aged device.')
    # A1
    P('\n## A1: IU size (proposed at 18.75% DRAM; table = 18.75% - 4/U of the page map), IU+RMW at the same U')
    P(f'{"workload":16s}' + ''.join(f'{h:>9s}' for h in ['P U8', 'P U16', 'P U32', 'IU U8', 'IU U16', 'IU U32', 'page4k', 'bestLD']))
    for w in WLS:
        best = min([x for x in (W(w, 'dftl_b1875'), W(w, 'leaftl_b1875'), W(w, 'mu_b1875')) if x], default=None)
        P(f'{w:16s}' + ''.join(fmt(W(w, t), 9) for t in ['prop_U8_b1875', 'prop_U16_b1875', 'prop_U32_b1875', 'iurmw_U8', 'iurmw_U16', 'iurmw_U32', 'page4k']) + fmt(best, 9))
    # A2
    P('\n## A2: mapping-DRAM budget (% of the 4 B page map). proposed best-U shown with U in [], page4k = 100%')
    for pol, lab in (('prop', 'proposed U=16'), ('prop32', 'proposed U=32'), ('prop8', 'proposed U=8'), ('dftl', 'DFTL'), ('leaftl', 'LeaFTL'), ('mu', 'uFTL')):
        P(f'\n### {lab}')
        P(f'{"workload":16s}' + ''.join(f'{b / 100:>8.2f}%' for b in BUDGETS) + f'{"page4k":>9s}')
        for w in WLS:
            row = []
            for b in BUDGETS:
                t = {'prop': f'prop_U16_b{b}', 'prop32': f'prop_U32_b{b}', 'prop8': f'prop_U8_b{b}'}.get(pol, f'{pol}_b{b}')
                row.append(W(w, t))
            P(f'{w:16s}' + ''.join(fmt(x, 9) for x in row) + fmt(W(w, 'page4k'), 9))
    P('\n### proposed (best U at each budget) vs best low-DRAM baseline at the same budget: WAF ratio (proposed / best of DFTL, LeaFTL, uFTL)')
    P(f'{"workload":16s}' + ''.join(f'{b / 100:>12.2f}%' for b in BUDGETS))
    for w in WLS:
        cells = []
        for b in BUDGETS:
            ps = [(W(w, f'prop_U{u}_b{b}'), u) for u in (8, 16, 32)]
            ps = [x for x in ps if x[0]]
            bl = [x for x in (W(w, f'dftl_b{b}'), W(w, f'leaftl_b{b}'), W(w, f'mu_b{b}')) if x]
            if ps and bl:
                pv, u = min(ps); cells.append(f'{pv / min(bl):7.3f}[U{u:<2d}]')
            else:
                cells.append(f'{"-":>12s}')
        P(f'{w:16s}' + ' '.join(cells))
    # A3
    P('\n## A3: OP (fine_quota = 0.0429 x OP/0.07 of the blocks; fixq = 0.0429 kept); DRAM 18.75%')
    hd = []
    for op in (7, 12, 20):
        hd += [f'P{op}', f'Pfq{op}', f'pg{op}', f'DF{op}', f'Lea{op}']
    P(f'{"workload":16s}' + ''.join(f'{h:>8s}' for h in hd))
    for w in WLS:
        row = []
        for op in (7, 12, 20):
            sfx = '' if op == 7 else f'_op{op}'
            row += [W(w, f'prop_U16_b1875{sfx}'), W(w, f'prop_U16_b1875_fixq{sfx}') if op != 7 else W(w, 'prop_U16_b1875'),
                    W(w, f'page4k{sfx}'), W(w, f'dftl_b1875{sfx}'), W(w, f'leaftl_b1875{sfx}')]
        P(f'{w:16s}' + ''.join(fmt(x, 8) for x in row))
    # A4
    P('\n## A4: write-buffer size (same buffer for all policies); OP 7%, DRAM 18.75%')
    hd = []
    for wb in WBUFS:
        hd += [f'P{wb}', f'pg{wb}', f'DF{wb}', f'Le{wb}', f'IU{wb}']
    P(f'{"workload":16s}' + ''.join(f'{h:>8s}' for h in hd))
    for w in WLS:
        row = []
        for wb in WBUFS:
            sfx = '' if wb == '1M' else f'_wb{wb}'
            row += [W(w, f'prop_U16_b1875{sfx}'), W(w, f'page4k{sfx}'), W(w, f'dftl_b1875{sfx}'), W(w, f'leaftl_b1875{sfx}'), W(w, f'iurmw_U16{sfx}')]
        P(f'{w:16s}' + ''.join(fmt(x, 8) for x in row))
    s = '\n'.join(L) + '\n'
    open(f'{OUT}/SUMMARY.txt', 'w').write(s)
    print(s)


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'table'
    sel = sys.argv[2:]
    if cmd == 'jobs':
        for wl, tag, args in all_jobs(sel):
            print(wl, tag, args)
    elif cmd == 'run':
        run(sel)
    else:
        table()
