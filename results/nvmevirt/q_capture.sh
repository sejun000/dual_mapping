#!/bin/bash
# Block-trace capture queue (page4k, ext4 application workloads, blktrace via CAPTURE_BLKTRACE=1):
# waits for results/nvmevirt/q_main3.sh to exit and the NVMeVirt device to be idle, runs the workloads with
# tools/apps/run_all.sh into results/apps_capture/, converts each trace with tools/apps/blk2csv into
# /mnt/nvme_mounted/dm_apptrace/<workload>/trace.{csv,json}, then runs tools/apps_trace_sim.sh if present.
# Start (from /home/sejun000/dual_mapping):
#   setsid nohup bash results/nvmevirt/q_capture.sh > results/apps_capture/q_capture.out 2>&1 < /dev/null &
cd /home/sejun000/dual_mapping || exit 1
OUT=/home/sejun000/dual_mapping/results/apps_capture
TRACE_ROOT=/mnt/nvme_mounted/dm_apptrace
WLS="ycsbb_d_t64 ycsba_d_t64 ycsbfstd_d_t64 fio_randwrite_d fileserverfs tpcc_d_t64"
NODE0_CPUS=0-92:2
mkdir -p "$OUT" "$TRACE_ROOT"
LOG="$OUT/q_capture.log"
log() { echo "[$(date '+%F %T')] $*" | tee -a "$LOG"; }

busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
main3_running() {   # the original PID (only while it still is q_main3.sh - no PID-reuse hang) or any q_main3.sh
  { kill -0 3266237 2>/dev/null && grep -qa 'q_main3' /proc/3266237/cmdline 2>/dev/null; } ||
    pgrep -f 'results/nvmevirt/q_main3\.sh' >/dev/null
}

log "q_capture start (pid $$)"
# B18 cache for the new results root (run_all.sh reads $RESULTS_ROOT/b18_${NVMEV_MEMMAP_SIZE}.txt) - no probe load
[[ -s "$OUT/b18_44G.txt" ]] || cp results/apps_ext4/b18_44G.txt "$OUT/b18_44G.txt"
log "B18 file: $(cat "$OUT/b18_44G.txt")"
# converter binary
if [[ ! -x tools/apps/blk2csv || tools/apps/blk2csv.c -nt tools/apps/blk2csv ]]; then
  gcc -O2 -o tools/apps/blk2csv tools/apps/blk2csv.c >> "$LOG" 2>&1 || { log "FATAL: cannot build tools/apps/blk2csv"; exit 1; }
fi

while main3_running; do sleep 60; done
log "q_main3.sh has exited"
while busy; do sleep 60; done
log "device idle; free space on /mnt/nvme_mounted: $(df -h --output=avail /mnt/nvme_mounted | tail -1 | tr -d ' ')"
avail_kb="$(df -k --output=avail /mnt/nvme_mounted | tail -1 | tr -d ' ')"
((avail_kb > 200 * 1024 * 1024)) || { log "FATAL: less than 200 GiB free on /mnt/nvme_mounted"; exit 1; }

log "run_all start: $WLS (page4k, CAPTURE_BLKTRACE=1)"
CAPTURE_BLKTRACE=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_capture WORKLOADS="$WLS" POLICIES="page4k" \
  tools/apps/run_all.sh >> results/apps_capture/run_all.log 2>&1
log "run_all exit $?"

for wl in $WLS; do
  d="$TRACE_ROOT/$wl"
  st="$(cat "$OUT/$wl/page4k/STATUS" 2>/dev/null || echo missing)"
  if ! ls "$d"/trace.blktrace.* >/dev/null 2>&1; then log "$wl: no trace in $d (case STATUS $st)"; continue; fi
  log "$wl: case STATUS $st; converting $d ($(du -sh "$d" | cut -f1))"
  taskset -c "$NODE0_CPUS" tools/apps/blk2csv "$d" >> "$LOG" 2>&1
  rc=$?
  log "$wl: blk2csv exit $rc; $(python3 -c "import json,sys; j=json.load(open(sys.argv[1])); print('write_pages', j['write_pages'], 'read_pages', j['read_pages'], 'warmup_write_pages', j['warmup_write_pages'], 'prefill_done_write_pages', j['prefill_done_write_pages'], 'dropped', j['dropped_events'], 'markers', [m['name'] for m in j['markers']])" "$d/trace.json" 2>&1)"
done

if [[ -x tools/apps_trace_sim.sh ]]; then
  log "tools/apps_trace_sim.sh $TRACE_ROOT start"
  tools/apps_trace_sim.sh "$TRACE_ROOT" >> "$LOG" 2>&1
  log "tools/apps_trace_sim.sh exit $?"
else
  log "tools/apps_trace_sim.sh not present/executable - skipped"
fi
log "q_capture done"
