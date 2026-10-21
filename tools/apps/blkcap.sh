# tools/apps/blkcap.sh - opt-in block-level trace capture for run_app.sh (CAPTURE_BLKTRACE=1).
# Sourced by run_app.sh ONLY when CAPTURE_BLKTRACE=1, after common.sh and the workload plugin; not standalone.
#
# What it does:
#   blkcap_start  (run_app.sh: after the target device is identified, right BEFORE mkfs_and_mount, so the trace
#                  holds the mkfs writes and the never-written LBA space can be told from the trace)
#                 -> sudo blktrace -d $DEV -a issue -a requeue -a notify (D events of every request, reads and
#                    writes; requeues; message notes), binary per-CPU files <dir>/trace.blktrace.<cpu>.
#   markers       blktrace message events written to /sys/kernel/debug/block/<dev>/msg, each also logged with
#                 wall clock, /proc/uptime and /sys/block/<dev>/stat in <dir>/markers.txt (fallback):
#                   dm_blkcap_start          trace running (before mkfs)
#                   dm_measure_start         wraps capture_start (also when a plugin calls it itself: filebench)
#                   dm_measure_prefill_done  wraps dm_snapshot "measure" (fio: after its 30 GiB prefill + sync)
#                   dm_measure_end           wraps capture_end (before the end snapshot)
#   blkcap_stop   right after capture_end, and from run_app.sh's finish() if still running (before umount/rmmod):
#                 SIGINT to the blktrace PID (only that PID), wait for it to flush and exit.
# Output dir: ${BLKTRACE_DIR:-/mnt/nvme_mounted/dm_apptrace/<workload>} (an existing one is moved to .old.<ts>):
#   trace.blktrace.<cpu>, blktrace.log (blktrace stdout/stderr incl. per-CPU and dropped-event counts),
#   blkcap_meta.txt (key=value), markers.txt. Convert with tools/apps/blk2csv <dir>.
# Knobs: BLKCAP_CPUS (taskset for blktrace's main thread, default node-0 CPUs without 94; blktrace pins each per-CPU
# reader thread to its own CPU itself), BLKCAP_BUF_KB / BLKCAP_NBUF (relay sub-buffer size / count per CPU).

BLKTRACE_BIN="${BLKTRACE_BIN:-/usr/sbin/blktrace}"
BLKCAP_DIR="${BLKTRACE_DIR:-/mnt/nvme_mounted/dm_apptrace/$WL}"
BLKCAP_CPUS="${BLKCAP_CPUS:-0-92:2}"
BLKCAP_BUF_KB="${BLKCAP_BUF_KB:-128}"    # 128 KiB x 256 = 32 MiB relay buffer per CPU (expected peak ~10 MB/s in total)
BLKCAP_NBUF="${BLKCAP_NBUF:-256}"
BLKCAP_PROBE_CPU="${BLKCAP_PROBE_CPU:-0}"
BLKCAP_SUDO_PID=""; BLKCAP_PID=""; BLKCAP_DBG=""

blkcap_meta() { echo "$1=$2" >> "$BLKCAP_DIR/blkcap_meta.txt"; }

blkcap_snap() {   # $1 name, $2 status: one fallback line per marker
  printf '%s wall=%s uptime=%s msg=%s block_stat=%s\n' "$1" "$(date +%s.%N)" "$(cut -d' ' -f1 /proc/uptime)" "$2" \
    "$(tr -s ' ' < "/sys/block/$DEVNAME/stat" | sed 's/^ //')" >> "$BLKCAP_DIR/markers.txt"
}

blkcap_running() { [[ -n "$BLKCAP_SUDO_PID" ]] && kill -0 "$BLKCAP_SUDO_PID" 2>/dev/null; }

blkcap_mark() {   # $1 marker text -> blktrace message event + fallback snapshot
  local st=ok
  if blkcap_running; then
    printf '%s' "$1" | sudo tee "$BLKCAP_DBG/msg" >/dev/null 2>&1 || st=write_failed
  else
    st=not_tracing
  fi
  blkcap_snap "$1" "$st"
  log "[blkcap] marker $1 ($st)"
}

blkcap_start() {
  local i
  [[ -x "$BLKTRACE_BIN" ]] || fail "blkcap: missing $BLKTRACE_BIN"
  [[ -n "${DEV:-}" && -n "${DEVNAME:-}" ]] || fail "blkcap: target device not set"
  findmnt -rn /sys/kernel/debug >/dev/null 2>&1 || sudo mount -t debugfs debugfs /sys/kernel/debug ||
    fail "blkcap: cannot mount debugfs"
  BLKCAP_DBG="/sys/kernel/debug/block/$DEVNAME"
  sudo test -e "$BLKCAP_DBG/msg" && fail "blkcap: $DEV is already being traced ($BLKCAP_DBG/msg exists)"
  if [[ -e "$BLKCAP_DIR" ]]; then
    sudo mv "$BLKCAP_DIR" "$BLKCAP_DIR.old.$(date -u +%Y%m%dT%H%M%SZ)" || fail "blkcap: cannot move old $BLKCAP_DIR"
  fi
  mkdir -p "$BLKCAP_DIR" 2>/dev/null || { sudo mkdir -p "$BLKCAP_DIR" && sudo chown "$USER:$USER" "$BLKCAP_DIR"; } ||
    fail "blkcap: cannot create $BLKCAP_DIR"
  {
    echo "workload=$WL"; echo "policy=$POL"; echo "run_id=$RUN_ID"; echo "case_dir=$CASE_DIR"
    echo "loop_mode=${LOOP_MODE:-0}"; echo "device=$DEV"; echo "devname=$DEVNAME"
    echo "dev_majmin=$(cat "/sys/block/$DEVNAME/dev")"
    echo "device_bytes=$(sudo blockdev --getsize64 "$DEV")"
    echo "logical_block_size=$(cat "/sys/block/$DEVNAME/queue/logical_block_size")"
    echo "max_sectors_kb=$(cat "/sys/block/$DEVNAME/queue/max_sectors_kb")"
    echo "scheduler=$(cat "/sys/block/$DEVNAME/queue/scheduler")"
    echo "blktrace=$("$BLKTRACE_BIN" -V 2>&1 | head -1)"
    echo "blktrace_args=-d $DEV -D $BLKCAP_DIR -o trace -b $BLKCAP_BUF_KB -n $BLKCAP_NBUF -a issue -a requeue -a notify"
    echo "blktrace_taskset=$BLKCAP_CPUS"
    echo "trace_start_point=before mkfs (mkfs, mount, dataset load, ballast are warm-up; measurement starts at marker dm_measure_start)"
    echo "start_wall=$(date +%s.%N)"; echo "start_block_stat=$(tr -s ' ' < "/sys/block/$DEVNAME/stat" | sed 's/^ //')"
  } > "$BLKCAP_DIR/blkcap_meta.txt"
  # bash -c records the PID that becomes blktrace (exec chain) so only that PID is ever signalled; fd 9 (the run
  # lock) is not inherited
  sudo bash -c 'echo $$ > "$1/blktrace.pid"; exec taskset -c "$2" "$3" -d "$4" -D "$1" -o trace -b "$5" -n "$6" -a issue -a requeue -a notify' \
    _ "$BLKCAP_DIR" "$BLKCAP_CPUS" "$BLKTRACE_BIN" "$DEV" "$BLKCAP_BUF_KB" "$BLKCAP_NBUF" \
    > "$BLKCAP_DIR/blktrace.log" 2>&1 < /dev/null 9>&- &
  BLKCAP_SUDO_PID=$!
  for i in $(seq 200); do
    blkcap_running || break
    [[ -s "$BLKCAP_DIR/blktrace.pid" ]] && sudo test -e "$BLKCAP_DBG/msg" && ls "$BLKCAP_DIR"/trace.blktrace.* >/dev/null 2>&1 && break
    sleep 0.1
  done
  BLKCAP_PID="$(cat "$BLKCAP_DIR/blktrace.pid" 2>/dev/null)"
  if ! blkcap_running || [[ -z "$BLKCAP_PID" ]] || ! sudo test -e "$BLKCAP_DBG/msg"; then
    cat "$BLKCAP_DIR/blktrace.log"; blkcap_stop; fail "blkcap: blktrace did not start (see $BLKCAP_DIR/blktrace.log)"
  fi
  blkcap_meta blktrace_pid "$BLKCAP_PID"
  # blktrace issues BLKTRACESTART only once every per-CPU reader thread runs on its CPU - that can take seconds when
  # a CPU is busy-polled by NVMeVirt - and messages/events before it are silently dropped. Its readers only see full
  # relay sub-buffers, so: from CPU $BLKCAP_PROBE_CPU write batches of probe messages (> one sub-buffer per batch)
  # until one reaches trace.blktrace.$BLKCAP_PROBE_CPU; the trace is then provably live before mkfs.
  # blk2csv ignores the "dm_blkcap_probe" messages (counted as probe_messages).
  for i in $(seq 300); do
    blkcap_running || break
    sudo taskset -c "$BLKCAP_PROBE_CPU" sh -c 'n=0; while [ $n -lt "$2" ]; do
        printf "dm_blkcap_probe_%06d_padding_padding_padding_padding_padding_padding_padding_padding_padding_" $n > "$1" || exit 1
        n=$((n + 1)); done' _ "$BLKCAP_DBG/msg" $((BLKCAP_BUF_KB * 1024 / 150 + 64)) || true
    sleep 1
    grep -qaF dm_blkcap_probe "$BLKCAP_DIR/trace.blktrace.$BLKCAP_PROBE_CPU" 2>/dev/null && break
  done
  if ! grep -qaF dm_blkcap_probe "$BLKCAP_DIR/trace.blktrace.$BLKCAP_PROBE_CPU" 2>/dev/null; then
    blkcap_stop; fail "blkcap: probe messages never reached the trace files (see $BLKCAP_DIR/blktrace.log)"
  fi
  blkcap_meta probe_batches "$i"
  log "[blkcap] blktrace pid $BLKCAP_PID on $DEV -> $BLKCAP_DIR (-b $BLKCAP_BUF_KB -n $BLKCAP_NBUF), live after $i probe batch(es)"
  blkcap_mark dm_blkcap_start
}

blkcap_stop() {   # idempotent; SIGINT to blktrace itself (by its recorded PID only), wait for flush + exit
  local i d
  [[ -n "$BLKCAP_SUDO_PID" ]] || return 0
  if blkcap_running; then
    d="$(sudo cat "$BLKCAP_DBG/dropped" 2>/dev/null)"; blkcap_meta dropped_debugfs_before_stop "${d:-?}"
    blkcap_snap dm_blkcap_stop sigint
    [[ -n "$BLKCAP_PID" ]] && sudo kill -INT "$BLKCAP_PID" 2>/dev/null
    for i in $(seq 600); do blkcap_running || break; sleep 0.5; done
    if blkcap_running; then
      log "WARNING: blktrace (pid $BLKCAP_PID) did not exit within 300 s of SIGINT; SIGTERM"
      sudo kill -TERM "$BLKCAP_PID" 2>/dev/null
      for i in $(seq 60); do blkcap_running || break; sleep 0.5; done
    fi
    if blkcap_running; then
      log "WARNING: blktrace still running; SIGKILL pid $BLKCAP_PID and tear the trace down (blktrace -k)"
      sudo kill -KILL "$BLKCAP_PID" 2>/dev/null; sleep 1
      sudo "$BLKTRACE_BIN" -k -d "$DEV" >> "$BLKCAP_DIR/blktrace.log" 2>&1
      echo "blktrace_killed=1" >> "$CASE_DIR/warnings.txt"; blkcap_meta blktrace_killed 1
    fi
  fi
  wait "$BLKCAP_SUDO_PID" 2>/dev/null; blkcap_meta blktrace_exit "$?"
  BLKCAP_SUDO_PID=""
  d="$(sed -n 's/.*(dropped \([0-9]*\)).*/\1/p' "$BLKCAP_DIR/blktrace.log" | awk '{ s += $1 } END { print s + 0 }')"
  blkcap_meta dropped_events "$d"
  blkcap_meta stop_wall "$(date +%s.%N)"
  blkcap_meta stop_block_stat "$(tr -s ' ' < "/sys/block/$DEVNAME/stat" 2>/dev/null | sed 's/^ //')"
  blkcap_meta trace_bytes "$(cat "$BLKCAP_DIR"/trace.blktrace.* 2>/dev/null | wc -c)"
  sudo chown -R "$USER:$USER" "$BLKCAP_DIR" 2>/dev/null
  ((d == 0)) || { log "WARNING: blktrace dropped $d events"; echo "blktrace_dropped=$d" >> "$CASE_DIR/warnings.txt"; }
  if sudo test -e "$BLKCAP_DBG/msg"; then log "WARNING: $BLKCAP_DBG still exists after blktrace exit"; fi
  log "[blkcap] stopped; $(grep -m1 'Total:' "$BLKCAP_DIR/blktrace.log" | sed 's/^ *//')"
}

# ---------------------------------------------------------------- hooks (wrap common.sh functions)
eval "$(declare -f capture_start | sed '1s/^capture_start /blkcap_orig_capture_start /')"
eval "$(declare -f capture_end | sed '1s/^capture_end /blkcap_orig_capture_end /')"
eval "$(declare -f dm_snapshot | sed '1s/^dm_snapshot /blkcap_orig_dm_snapshot /')"
capture_start() { blkcap_mark dm_measure_start; blkcap_orig_capture_start "$@"; }
capture_end() { local rc; blkcap_mark dm_measure_end; blkcap_orig_capture_end "$@"; rc=$?; blkcap_stop; return $rc; }
dm_snapshot() { [[ "${1:-}" == measure ]] && blkcap_mark dm_measure_prefill_done; blkcap_orig_dm_snapshot "$@"; }
