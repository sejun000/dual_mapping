#!/bin/bash
# DOGI (FAST'26) on the V3 evaluation matrix (results/dogi_v3; summary: tools/dogi_v3_summary.py).
#   MODES="strict" XTAG=_sh tools/run_dogi_v3.sh <name> <P> <NB> "<ftlsim workload args>"
#   (job lines: results/dogi_v3/jobs.txt; queue: tools/run_dogi_v3_queue.sh)
# ftlsim (private copy of ftlsim.cpp that writes a MARK line at the measurement reset, $FTLSIM) replays the workload
# exactly as results/op7b / results/sens did, in mode=page4k, and exports its post-buffer host stream (1 MiB
# IU-coalescing write buffer, one line per 4K page) into a FIFO; DOGI (third_party/dogi_port, segment = P pages,
# binary built per P on first use) consumes it live. The page4k json of the export run is the WAF denominator
# (host_pages of the window) and the check that the stream equals the reference one.
# Modes: strict = at most NB segments hold data (sealed + open + GC victim); foreground GC while free <= RESERVE;
#                 dead-end liveness guard (see scheduler.cc). DOGI_GUARD_ANY=1 -> "DOGI+" (never collect a
#                 fully-valid segment).
#        gp     = artifact accounting (GC when garbage of sealed segments > OP/(1+OP); open segments outside the
#                 budget). DOGI_GUARD=1 enables the same guard (results "gp_gh").
#        gpadj  = gp with OP lowered by the 6 open segments, OP = (NB-6)P/L - 1.
# XTAG: suffix for log/json names (dogi_<mode><XTAG>.log, page4k_export<XTAG>.json, run<XTAG>.txt) and the sandbox.
# Run tags used in results/dogi_v3: strict/strict_r2 (no guard), strict_sh/_sh2/_sh3 (dead-end guard),
# strict_se/_se2 (DOGI_GUARD_ANY=1), gp/gp_ge (gp, no guard), gp_gh (gp + DOGI_GUARD=1), gpadj (no guard).
set -u
NAME=$1; P=$2; NB=$3; WL=$4
REPO=$(cd "$(dirname "$0")/.." && pwd)
SCR=${SCR:-/tmp/claude-1001/-home-sejun000-dual-mapping/64054dd3-9fa1-4be8-986a-b7744c0b8ecf/scratchpad}
FTLSIM=${FTLSIM:-$REPO/results/dogi_v3/ftlsim_x.bin}   # ftlsim.cpp snapshot + results/dogi_v3/ftlsim_out_trace_mark.patch
DOGI=$SCR/dogi_P$P
OUT=$REPO/results/dogi_v3/$NAME
MODES=${MODES:-strict}
RESERVE=${RESERVE:-2}
CPUS=${CPUS:-0-92:2}
mkdir -p "$OUT"
[ -x "$DOGI" ] || (SEG=$P OUT=$DOGI "$REPO/third_party/dogi_port/build.sh" > "$OUT/build_$P.log" 2>&1)
[ -x "$DOGI" ] || { echo "no DOGI binary for P=$P" >&2; exit 1; }
# logical pages L as ftlsim derives them (capacity / 4096, 64 GiB default)
L=$(python3 - "$WL" <<'PY'
import sys
cap = 64 << 30
for a in sys.argv[1].split():
    if a.startswith('capacity='): cap = int(a.split('=')[1])
print(cap // 4096)
PY
)
OPX=$(python3 -c "print(repr($NB*$P/$L-1))")
# sandboxes (training CSVs, models) live off the root fs
RUNS=${RUNS:-/mnt/nvme_mounted/dogi_v3_runs}
XTAG=${XTAG:-}
W=$RUNS/$NAME$XTAG; rm -rf "$W"; mkdir -p "$W"
FIFO=$W/stream.fifo; mkfifo "$FIFO"
pids=()
fifos=()
for m in $MODES; do
  R=$W/$m; mkdir -p $R/work $R/DOGI-Train/TrainedModel $R/DOGI-Gconf/DOGI-BIR $R/DOGI-Gconf/DOGI-HOT $R/DOGI-Gconf/DOGI-PLog
  # artifact trainer pins itself to CPU 0 (all concurrent runs would share it): keep the inherited affinity instead
  sed "s/os.sched_setaffinity(0, {0})/pass  # port: inherited affinity/" "$REPO/third_party/dogi_port/DOGI-Train/model_trainer.py" > $R/DOGI-Train/model_trainer.py
  ln -s "$HOME/venv" $R/venv
  mkfifo $R/in.fifo; fifos+=($R/in.fifo)
  NSEG=$NB; OPM=$OPX
  [ $m = gp ] && NSEG=0
  [ $m = gpadj ] && { NSEG=0; OPM=$(python3 -c "print(repr(($NB-6)*$P/$L-1))"); }
  ( cd $R/work && DOGI_LOGICAL_BYTES=$((L * 4096)) DOGI_OP=$OPM DOGI_NSEG=$NSEG DOGI_GC_RESERVE=$RESERVE \
      OMP_NUM_THREADS=2 MKL_NUM_THREADS=2 taskset -c $CPUS /usr/bin/time -v "$DOGI" DOGI $R/in.fifo \
      > "$OUT/dogi_$m$XTAG.log" 2> "$OUT/dogi_$m$XTAG.err" ) &
  pids+=($!)
done
# fan the single export out to every DOGI instance
if [ ${#fifos[@]} -eq 1 ]; then cat "$FIFO" > ${fifos[0]} & else cat "$FIFO" | tee ${fifos[@]:1} > ${fifos[0]} & fi
tpid=$!
RT="$OUT/run$XTAG.txt"
echo "[run_dogi_v3] $NAME P=$P NB=$NB L=$L DOGI_OP=$OPX modes=$MODES reserve=$RESERVE start $(date)" > "$RT"
taskset -c $CPUS /usr/bin/time -v "$FTLSIM" $WL mode=page4k tag=page4k out_trace="$FIFO" > "$OUT/page4k_export$XTAG.json" 2> "$OUT/page4k_export$XTAG.log"
wait $tpid
for p in "${pids[@]}"; do wait $p; done
echo "end $(date)" >> "$RT"
EXPORT="$OUT/page4k_export$XTAG.json" python3 "$REPO/tools/dogi_v3_waf.py" "$OUT" $(for m in $MODES; do echo $m$XTAG; done) >> "$RT"
rm -rf "$W"
