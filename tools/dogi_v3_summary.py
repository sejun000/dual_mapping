#!/usr/bin/env python3
"""results/dogi_v3/SUMMARY.txt: DOGI WAF next to the V3 matrix numbers + DOGI DRAM model per geometry.
Full-device workloads: reference numbers from results/op7b (V3_18, page4k, dftl12, leaftl12, sepbit).
Captured apps (aged): reference numbers from results/sens (prop_U16_b1875, page4k, dftl_b1875, leaftl_b1875)."""
import json, os, math

R = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'results')
D = os.path.join(R, 'dogi_v3')
FULL = ['z0', 'z0.8', 'z0.99', 'z1.2', 'ali1', 'ali2', 'ali3', 'ssd', 'kv_w1', 'spfresh']
APPS = ['ycsbb_d_t64', 'ycsba_d_t64', 'ycsbfstd_d_t64', 'tpcc_d_t64', 'fio_randwrite_d', 'fileserverfs']


def jl(p):
    try:
        return json.load(open(p))
    except Exception:
        return None


def waf(p):
    j = jl(p)
    return j['WAF'] if j else float('nan')


def f(x, w=8):
    return f'{x:{w}.3f}' if isinstance(x, (int, float)) and not math.isnan(x) else f'{"-":>{w}s}'


def pick(w, *names):
    """first existing run json among names (e.g. the fallback-binary rerun before the original run)"""
    for n in names:
        j = jl(f'{D}/{w}/dogi_{n}.json')
        if j and 'WAF' in j:
            j['_name'] = n
            return j
    return {}


def ml(j):
    if not j or 'WAF' not in j:
        return 'n/a'
    s = j['ml_status']
    return f"{s}, {j['n_trainings_started']} trainings ({j['trainings_started_in_window']} in window)"


NOTE = """
Framework constraints: one dm/ftl_<policy>.c behind dm_ftl_ops, compiled both in the kernel and in the userspace
harness, host writes arrive after the shared dm_wbuf, GC must be the foreground free-line-threshold GC (T=2) of the
glue, no FPU unless the file is built with CC_FLAGS_FPU and every FP region is wrapped in kernel_fpu_begin/end (no
sleeping/allocation inside), vzalloc only in create(), and the port is validated bit-identically against a
deterministic userspace reference (ftlsim mode=...).
Easy parts: the placement/GC engine (6 open lines = groups, hot filter from the per-LBA rewrite-interval byte,
FrozenFilter bit, first-GC category -> group map, BIR-age victim rule for the BIR groups, cost-benefit
gp/(1-gp)*sqrt(age) for the rest -> integer cross-multiplication with dm_isqrt, as already done for CB policies).
The per-4K metadata (intervals 1 B/LBA, freqBits 1 B/physical page, 1 bit frozen, 4-bit category per physical page
that can live next to rmap[]) is ~24 MB at the 44G device and ~2.3 B/LBA in general: vzalloc'able. Two O(L)
sweeps sit in the write path (freqBits window advance every 8 Mi writes clears one bit of every LBA; chunkFreq
halving every 20 GiB) and would need to be made incremental/epoch-based in the kernel to avoid ms-scale stalls.
MLP inference (6-32-32-10 float, 1.6k MAC per write, batched 128 non-hot writes per 16 MiB buffer in the artifact)
is cheap; in the kernel it needs an FPU section per batch or an int16 fixed-point rewrite (then it is no longer
bit-identical with the float reference).
Hard parts: (1) online training -- the artifact samples 400k feature rows + ~8M follow-up LBAs to CSV files, then
shells out to Python/PyTorch (15 epochs, StandardScaler, class thresholds), reloads W*.bin, and runs the group
optimizer (Markov-chain enumeration of K / m_list / BIR in doubles over a PLog CSV). None of this can run in the
kernel I/O path; it needs a userspace helper (export samples via a char device / relay buffer, train, push weights
+ K/BIR back via ioctl/sysfs). (2) Determinism -- the artifact is multi-threaded (ingest, ML worker, background GC,
trainer) and its model hand-off is wall-clock dependent; the kernel port has no deterministic reference to be
validated against unless the userspace port is first refactored into a single-threaded, synchronous-GC,
synchronous-train form (our DOGI-strict + sync-training run is already close to that). (3) The Markov group
optimizer is doubles-heavy and file-based: userspace only.
Faithful variants: an OFFLINE-TRAINED DOGI (weights + K/m_list/BIR frozen from the userspace run's last training
round on the same workload, loaded at module init; inference in an FPU section; placement/GC as above) is a
reasonable and faithful baseline for NVMeVirt latency/throughput, if labelled as such (it is optimistic: trained
on the same trace, no adaptation lag). Caveat from the userspace runs: K/BIR are not always stable across
retrains (z0.99 switches K=3 <-> 4, z0 settles at K=1, traces wander), and one late switch can move the window WAF
by 2x, so a frozen configuration is a single sample of DOGI's behaviour -- report it next to the userspace spread.
The glue's foreground GC at T=2 is exactly our DOGI-strict regime: the port must include the dead-end liveness
guard (or DOGI livelocks when its cost-benefit pool is fully valid and the BIR clock is frozen). A NON-ML variant (APPLY_ML=0: hot filter + single non-hot group + CB) is NOT DOGI -- it
drops the invalidation-time prediction and dynamic grouping that define it -- and should not be presented as DOGI.
Estimate: offline-trained port ~1-2 weeks (incl. userspace reference refactor); online training via a helper
daemon adds roughly another 1-2 weeks and gives up bit-identical validation.
"""
BOTTOM = """
- vs V3 18.75%: best-DOGI is lower on z0 (-23%), z1.2 (-20%), ali3 (-8%), spfresh (-60%) and all six aged app traces
  (-10..-45%; V3 itself is above page4k there); V3 is lower on z0.8 (DOGI +172%), z0.99 (+13%), ali1 (+25%),
  ali2 (+18%), kv_w1 (+3%); ssd is a tie (+0.3%).
- vs page4k: best-DOGI is lower on z1.2 (-35%), ali3 (-24%), spfresh (-26%), fileserverfs (-15%), ssd (-3%), ali1 (-1%);
  higher on z0 (+18%), z0.8 (+197%), z0.99 (+12%), kv_w1 (+14%), ali2 (+2.5%) and five app traces (+0.3..+15%).
- DRAM: DOGI ~159% of the page4k map (167% with a DRAM-resident per-page category) vs V3 at 18.75%.
- DOGI is fragile at OP 7% under an equal physical budget: its 6 open groups plus garbage parked in BIR groups (not
  collectable before their BIR age) can exhaust the 24-segment slack, and GC then grinds through nearly-full
  segments (z0.8 20-33, faithful tpcc/ycsbb 14-24). With its open segments outside the budget (DOGI-gp, ~+25%
  effective OP) it is markedly better on most workloads (z0.99 4.6, z1.2 1.6, ssd 5.1), but that accounting also
  thrashes on some traces. Run-to-run spread is large (z0.99 6.4-12.7): the port is multi-threaded and the BIR/ML
  feedback amplifies small timing differences; deterministic workloads (kv_w1, z0, fio) repeat exactly.
- ML: every run trained (retrain count per row; ali3 81 trainings, 66 in the window). Full-device workloads except
  spfresh had a model applied before the window. spfresh and all app traces: the first model only completes inside
  the window (training starts after 2x device writes + ~12M collected writes), i.e. ML from mid-window; the
  window still contains 1-10 trainings. With the artifact's own collection rule (true-LBA follow-ups only from the
  hot path) low-hot workloads never train: results/dogi_v3/_variant_artifact_collection (ssd: no ML, z0.99: 5 trainings).
- No sampled traces were needed: full ali3 (2.41 TB device, 2.85 G stream pages) ran in 2-3 h at ~18 GB RSS.
"""
lines = []
P = lines.append


def rel(x, y):
    return f'{(x / y - 1) * 100:+6.1f}%' if not (math.isnan(x) or math.isnan(y)) else f'{"-":>7s}'


def runs(w, names):
    out = []
    for n in names:
        j = jl(f'{D}/{w}/dogi_{n}.json')
        if j and 'WAF' in j:
            j['_name'] = n
            out.append(j)
    return out


def thrash(j):
    # GC without progress: window WAF > 50 or footprint far beyond NB
    return j and 'WAF' in j and (j['WAF'] > 50 or j.get('raw', {}).get('END', {}).get('peak', 0) > j.get('NB', 1e9) + 8)


def ref_full(w):
    b = f'{R}/op7b/{w}'
    return dict(page4k=waf(f'{b}/page4k.json'), V3=waf(f'{b}/V3_18.json'), dftl=waf(f'{b}/dftl12.json'),
                leaftl=waf(f'{b}/leaftl12.json'), sepbit=waf(f'{b}/sepbit.json'))


def ref_app(w):
    b = f'{R}/sens/{w}'
    return dict(page4k=waf(f'{b}/page4k.json'), V3=waf(f'{b}/prop_U16_b1875.json'), dftl=waf(f'{b}/dftl_b1875.json'),
                leaftl=waf(f'{b}/leaftl_b1875.json'), sepbit=float('nan'))


def mlstat(j):
    if not j:
        return 'n/a'
    s = {'ML': 'ML before window', 'ML(late)': 'ML from mid-window', 'no-ML': 'no ML'}[j['ml_status']]
    return f"{s}; {j['n_trainings_started']} trainings ({j['trainings_started_in_window']} in window)"


P('DOGI (Kim et al., FAST\'26) on the V3 matrix -- artifact port third_party/dogi_port, online ML (PyTorch in ~/venv).')
P('Stream: ftlsim (private copy of the current source with a MARK line at the measurement reset; mode=page4k, same')
P('arguments as the reference runs) exports its post-buffer host stream (after the 1 MiB IU-coalescing write buffer,')
P('one line per 4K page) into a FIFO that DOGI consumes live. Check: the export run reproduces the reference page4k')
P('WAF and host_pages_ftl exactly, and DOGI\'s user writes after MARK == host_pages_ftl (section 3).')
P('WAF = (DOGI user + GC programs after MARK) / host pages submitted in the window (export json host_pages).')
P('Segment = ftlsim block (P pages, binary built per P); DOGI_LOGICAL_BYTES = L*4096, OP = NB*P/L - 1.')
P('')
P('DOGI      = equal physical space: at most NB segments hold data (sealed + open + GC victim), GC while free <= 2,')
P('            host stalls meanwhile (foreground GC). Dead-end guard only: if the DogiSelect pick comes from its cost-')
P('            benefit pool and is fully valid (the whole pool is valid, BIR groups not yet expired, and the BIR clock --')
P('            host writes -- is frozen during GC), take the sealed segment with the most invalid pages; the artifact')
P('            would relocate full segments forever there. Mean over runs [min..max] n (unguarded completed runs and')
P('            dead-end-guard runs; DOGI is multi-threaded and its BIR/ML feedback is chaotic -> large run-to-run spread).')
P('DOGI+     = same, but NEVER collects a fully-valid victim (also overrides fully-valid expired-BIR picks, which DOGI')
P('            relocates by design) -> an optimistic, stronger-than-published DOGI; use the better of the two when')
P('            claiming wins (best/... columns).')
P('DOGI-gp   = artifact accounting: GC when garbage of SEALED segments > OP/(1+OP); its 6 open segments are outside')
P('            the budget (peak footprint NB+5..7, i.e. ~+25% OP), dead-end guard. Closest to the published setup;')
P('            NOT equal space. ! = GC without progress (window WAF > 50 or footprint > NB+8).')
P('Common port changes: synchronous model hand-off (stream pauses while PyTorch trains, so the model applies at the')
P('stream position where collection ended -- favours DOGI, no adaptation lag); after the 400k-sample phase every')
P('follow-up write is logged as a true-LBA record (artifact: hot-path only, so low-hot workloads -- uniform, large-')
P('segment traces -- never finished collecting and never trained); per-LBA tables sized to the device.')
P('')

ST = ['strict', 'strict_r2', 'strict_sh', 'strict_sh2', 'strict_sh3']   # faithful DOGI, equal space (unguarded completed + dead-end guard)
ST_V1 = ['strict_se', 'strict_se2']                         # DOGI+ : never collects a fully-valid victim


def stat(js):
    v = [j['WAF'] for j in js]
    if not v:
        return float('nan'), ''
    m = sum(v) / len(v)
    return m, (f'[{min(v):.2f}..{max(v):.2f}] {len(v)}' if len(v) > 1 else '1')


def table(wls, ref):
    P(f"{'workload':16s} {'DOGI':>7s} {'range n':>16s} {'DOGI+':>7s} {'range n':>16s} {'DOGI-gp':>8s} {'page4k':>7s} {'V3':>7s} {'DFTL':>7s} {'LeaFTL':>7s} {'SepBIT':>7s} {'best/p4k':>8s} {'best/V3':>8s}  ML (faithful runs)")
    rows = []
    for w in wls:
        st, v1 = runs(w, ST), runs(w, ST_V1)
        gg = (runs(w, ['gp_gh']) or [{}])[0]
        r = ref(w)
        a, ar = stat(st)
        b, br = stat(v1)
        best = min(x for x in (a, b, float('inf')) if not math.isnan(x)) if not (math.isnan(a) and math.isnan(b)) else float('nan')
        g = gg.get('WAF', float('nan'))
        gs = f(g, 8) + ('!' if thrash(gg) else ' ')
        mls = sorted(set(j['ml_status'] for j in st))
        tr = [j['n_trainings_started'] for j in st]
        mlt = (f"{'/'.join(mls)}; trainings {min(tr)}-{max(tr)}" if tr else 'n/a').replace('ML(late)', 'ML-mid-window')
        P(f"{w:16s} {f(a, 7)} {ar:>16s} {f(b, 7)} {br:>16s} {gs:9s}{f(r['page4k'], 7)} {f(r['V3'], 7)} {f(r['dftl'], 7)} {f(r['leaftl'], 7)} {f(r['sepbit'], 7)} "
          f"{rel(best, r['page4k']):>8s} {rel(best, r['V3']):>8s}  {mlt}")
        rows.append((w, st or v1, gg, r))
    return rows


P('== 1. Full-device workloads, OP 7% (results/op7b geometry and stream; refs from results/op7b, DRAM-limited')
P('      baselines at the 18.75% budget). Full traces for all workloads (no sampled traces needed).')
rows1 = table(FULL, ref_full)
P('')
P('== 2. Captured application traces, aged (full seq fill, identity addresses, NVMeVirt 44G geometry')
P('      L=10798160 P=30976 NB=373; window = capture markers). Refs from results/sens (OP 7%, 18.75%; SepBIT not run).')
rows2 = table(APPS, ref_app)
P('')
P('')
P('== Bottom line (equal physical space, OP 7%; "best" = better of the DOGI and DOGI+ means)')
for t in BOTTOM.strip('\n').split('\n'):
    P(t)
P('')
P('== 3. Checks, every run, and unguarded artifact-accounting variants')
P(f"{'workload':16s} {'P':>8s} {'NB':>4s} {'p4k exp':>8s} {'p4k ref':>8s} {'same':>5s} {'user==':>6s} {'peak':>5s} {'guard#':>7s} {'pk gp':>5s}  runs (WAF) | gp no-guard (peak) | gp OP-6seg no-guard (peak)")
TAGS = [('strict', 'u1'), ('strict_r2', 'u2'), ('strict_sh', 'g1'), ('strict_sh2', 'g2'), ('strict_sh3', 'g3'), ('strict_se', 'p1'), ('strict_se2', 'p2')]
for w, st, gg, r in rows1 + rows2:
    ex = next((x for x in (jl(f'{D}/{w}/page4k_export{t}.json') for t in ('_sh', '_se', '', '_sh2', '_gh', '_ge', '_gpadj')) if x), {})
    same = abs(ex.get('WAF', -1) - r['page4k']) < 5e-4
    allst = runs(w, [t for t, _ in TAGS])
    peak = max((j.get('raw', {}).get('END', {}).get('peak', -1) for j in allst), default=-1)
    gpk = gg.get('raw', {}).get('END', {}).get('peak', -1)
    guard = ','.join(str(j.get('raw', {}).get('END', {}).get('emergency', 0)) for j in runs(w, ['strict_sh', 'strict_sh2', 'strict_sh3'])) or '-'
    rr = ' '.join(f"{lab}={j['WAF']:.2f}" for t, lab in TAGS for j in runs(w, [t]))
    gpu = (runs(w, ['gp_ge', 'gp']) or [None])[0]
    adj = (runs(w, ['gpadj']) or [None])[0]
    gps = f"{gpu['WAF']:.2f}({gpu['raw']['END'].get('peak')})" if gpu else '-'
    ads = f"{adj['WAF']:.2f}({adj['raw']['END'].get('peak')})" if adj else '-'
    uok = all(j.get('user_window_matches_export') for j in allst) if allst else '-'
    P(f"{w:16s} {ex.get('P', 0):8d} {ex.get('NB', 0):4d} {f(ex.get('WAF', float('nan')))} {f(r['page4k'])} {str(same):>5s} "
      f"{str(uok):>6s} {peak:5d} {guard:>7s} {gpk:5d}  {rr} | {gps} | {ads}")
P('  same = the export run reproduces the reference page4k WAF; user== = DOGI user writes after MARK equal the export')
P('  host_pages_ftl in every run; peak = max segments holding data over all equal-space runs (must be <= NB);')
P('  guard# = dead-end guard picks in g1,g2; pk gp = DOGI-gp peak footprint. Runs: u1/u2 = no guard (completed),')
P('  g1/g2 = dead-end guard, p1/p2 = DOGI+ guard. Artifact accounting without any guard thrashes (GC without')
P('  progress) on several traces (values > 50).')
P('')
# ---- DRAM model ----
GEO = {  # name: (L logical pages, NB*P physical pages)
    '64 GiB synthetic': (16777216, 365 * 49152),
    'ali1 (257.6 GB)': (67530752, 365 * 197632),
    'ali2 (343.4 GB)': (90015232, 365 * 263168),
    'ali3 (2410 GB)': (631823616, 365 * 1850368),
    'ssd (394.5 GB)': (103404032, 366 * 302080),
    'kv_w1 (62.4 GB)': (16357888, 371 * 47104),
    'spfresh (40.0 GB)': (10490880, 365 * 30720),
    'apps 44G NVMeVirt': (10798160, 373 * 30976),
}
MLP = (6 * 32 + 32 + 32 * 32 + 32 + 32 * 10 + 10 + 12) * 4   # weights + biases + scaler, float32
P('== 4. DOGI DRAM model (information needed by the algorithm, page map at 4 B/entry like page4k; artifact')
P('      implementation uses 8 B map entries and 8 B per FrozenFilter flag, i.e. more). Per-4K tables:')
P('      page map 4 B x L | HotIntervalTracker intervals_ 1 B x L | FreqFeatures freqBits_ 1 B x physical pages |')
P('      chunkFreq 8 B / 2 MiB chunk | FrozenFilter 1 bit x L | ModelTrainer sampled-LBA bitmap 1 bit x physical')
P(f'      pages (persistent member) | MLP 6-32-32-10 + scaler {MLP} B. Training working set (truelba hash map, <=400k')
P('      entries, ~13 MB transient) and the host-side PyTorch trainer / CSV files are NOT counted.')
P(f"{'geometry':20s} {'page4k MB':>10s} {'DOGI MB':>9s} {'DOGI/page4k':>11s} {'B/4K-LBA':>9s}   breakdown MB (map, intervals, freqBits, chunk, frozen, sampled)")
for g, (L, NP) in GEO.items():
    parts = [4 * L, L, NP, 8 * math.ceil(NP / 512), L / 8, NP / 8]
    tot = sum(parts) + MLP
    P(f"{g:20s} {4 * L / 1e6:10.1f} {tot / 1e6:9.1f} {tot / (4 * L) * 100:10.1f}% {tot / L:9.3f}   " + ', '.join(f'{x / 1e6:.1f}' for x in parts))
P('      (other policies, same axis: page4k 100%, V3 / DFTL / LeaFTL 18.75% budget, SepBIT 200% = map + 4 B last-write')
P('       timestamp per LBA, see SPEC v3.1 appendix).')
P('      Not counted (as for page4k): the P2L/reverse map used by GC. DOGI additionally tags every physical page with')
P('      its predicted category for the first-GC group mapping (artifact: int32 per page); if that lives in DRAM rather')
P('      than in the page OOB, add >= 0.5 B per physical page (4 bits): ~+8.5 points -> ~167% of page4k.')
P('')
# measured RSS of the artifact implementation
P('== 5. Measured max RSS of the artifact implementation (strict run; includes 8 B/entry map + 8 B FrozenFilter +')
P('      128 Mi-entry floors of the artifact tables + 8 B/physical page segment P2L/category arrays):')
for w in FULL + APPS:
    try:
        ef = next(x for x in (f'{D}/{w}/dogi_strict_sh.err', f'{D}/{w}/dogi_strict_se.err', f'{D}/{w}/dogi_strict.err') if os.path.exists(x))
        for ln in open(ef):
            if 'Maximum resident' in ln:
                P(f"  {w:16s} {int(ln.split(':')[1]) / 1e6:8.2f} GB")
    except Exception:
        pass

P('')
P('== 6. Feasibility note: porting DOGI into the NVMeVirt kernel FTL framework (~/nvmevirt_page4k, dm/ftlcore.h)')
for t in NOTE.strip('\n').split('\n'):
    P(t)
open(os.path.join(D, 'SUMMARY.txt'), 'w').write('\n'.join(lines) + '\n')
print('\n'.join(lines))
