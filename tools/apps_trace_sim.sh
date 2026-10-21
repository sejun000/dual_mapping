#!/bin/bash
# Replay captured application block traces (tools/apps/blkcap.sh + tools/apps/blk2csv) in ftlsim with ORIGINAL
# addresses (trace_remap=0) at the NVMeVirt 44G geometry (L from the captured device size, P = 30976, NB = 373).
#   tools/apps_trace_sim.sh <trace_root>      (e.g. /mnt/nvme_mounted/dm_apptrace; one dir per workload)
# Per workload, two device conditions:
#   fresh: no precondition (seq_fill=0) -- the trace (mkfs, dataset load, ballast) is the only thing ever written,
#          so never-written LBAs stay free, as in the results/apps_ext4 NVMeVirt runs;
#   aged : full sequential fill of the device first (every LBA mapped; the PREFILL_SEQ / no-TRIM aged SSD), then
#          the same trace (ext4's LBA stream does not depend on the device's prior contents).
# Warm-up = everything before the measurement marker (dm_measure_start; fio: dm_measure_prefill_done, as the
# NVMeVirt devmeas_* numbers); the replay stops at dm_measure_end. Policies: $POLS (tools/run_partial.sh names).
# Output: results/apps_trace_sim/<workload>/<cond>/<policy>.json + results/apps_trace_sim/SUMMARY.txt
cd "$(dirname "$0")/.."
ROOT=${1:-/mnt/nvme_mounted/dm_apptrace}
OUT=${OUT:-results/apps_trace_sim}
S=${SIM:-./ftlsim/ftlsim}
POLS=${POLS:-V3 V3F page4k dftl leaftl}
JOBS=${JOBS:-20}
CPUS=${CPUS:-0-92:2}
V3="mode=proposed U=16 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1 map_budget=0.1875"
FIX=${FIX:-fine_elastic=1}
jobs_list() {
  for j in "$ROOT"/*/trace.json; do
    [[ -s "$j" ]] || continue
    d=$(dirname "$j"); wl=$(basename "$d")
    read CAP WARM STOP < <(python3 - "$j" <<'PY'
import json, sys
j = json.load(open(sys.argv[1]))
m = {x['name']: x for x in j.get('markers', [])}
warm = m.get('dm_measure_prefill_done', m.get('dm_measure_start'))
stop = m.get('dm_measure_end')
print(j['capacity_bytes'], warm['write_pages_before'] if warm else -1, stop['write_pages_before'] if stop else -1)
PY
)
    L=$((CAP / 4096)); B18=$((L * 4 * 1875 / 10000))
    if ((L == 10798160)); then GEO="P=30976 op=0.070001602125"   # NVMeVirt 44G: 373 lines x 30976 pages
    else GEO="op=0.07"; echo "WARNING: $wl: device of $L pages is not the 44G NVMeVirt geometry; ftlsim auto P" >&2; fi
    for cond in fresh aged; do
      sf=0; [[ $cond == aged ]] && sf=1
      W="trace=$d/trace.csv trace_remap=0 seq_fill=$sf capacity=$CAP $GEO trace_warmup_pages=$WARM trace_stop_pages=$STOP"
      for p in $POLS; do
        case $p in
          V3) a="$V3" ;; V3F) a="$V3 $FIX" ;; page4k) a="mode=page4k" ;;
          dftl) a="mode=dftl cache_bytes=$B18 dftl_tpbuf=1" ;; leaftl) a="mode=leaftl cache_bytes=$B18" ;;
          sepbit) a="mode=sepbit" ;; *) continue ;;
        esac
        echo "$OUT/$wl/$cond $p $W $a"
      done
    done
  done
}
jobs_list | xargs -P $JOBS -L 1 bash -c 'o=$0; p=$1; shift 1; mkdir -p $o; taskset -c '"$CPUS"' '"$S"' "$@" tag=$p > $o/$p.json 2> $o/$p.log'
python3 - "$OUT" <<'PY'
import json, glob, os, sys
out = sys.argv[1]; rows = []
for d in sorted(glob.glob(out + '/*/*')):
    r = {}
    for f in glob.glob(d + '/*.json'):
        try: j = json.load(open(f))
        except Exception: continue
        p = j['prog']; h = j['host_pages']
        r[os.path.basename(f)[:-5]] = (j['WAF'], (p.get('finegc', 0) + p.get('cgc_clean', 0) + p.get('cgc_refold', 0) + p.get('pgc', 0)) / h,
                                       (p.get('fold', 0) + p.get('zerofill', 0)) / h)
    pols = ['V3', 'V3F', 'page4k', 'dftl', 'leaftl']
    rows.append(f"{os.path.relpath(d, out):28s} " + ' '.join(f"{k}={r[k][0]:.3f}(gc {r[k][1]:.2f} fold {r[k][2]:.2f})" for k in pols if k in r))
open(out + '/SUMMARY.txt', 'w').write('WAF (and GC / fold programs per submitted host page), ftlsim identity replay of captured traces\n' + '\n'.join(rows) + '\n')
print('\n'.join(rows))
PY
