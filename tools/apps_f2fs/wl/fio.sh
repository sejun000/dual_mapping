# wl/fio.sh - splitEval workloads/fio_seq.sh as configured by tests/mn_fio_queue.sh:
#   FIO_SEQ_SIZE_MB=30720 FIO_SEQ_ENGINE=psync FIO_SEQ_FDATASYNC=1 FIO_SEQ_NUMJOBS=16 FIO_SEQ_BS=4k
#   FIO_SEQ_RW=randwrite | randrw (FIO_SEQ_RWMIX=50), DURATION=300, FILL_TARGET_PERCENT=0
# One 30 GiB file shared by 16 psync jobs, buffered, fdatasync after every write, ramp_time 10 s.
# Random patterns are prefilled first (1 MiB libaio O_DIRECT, QD64, end_fsync).
# Study capture points are kept (start = before the prefill, as in the study, so its device WAF includes the
# prefill); an extra "measure" snapshot is taken after the prefill so the measured phase can be isolated.
# fio runs in the common memory cgroup (dmapp.slice) under numactl (the study had no cgroup for fio).

WL_KIND="fio-${FIO_RW:?}"
WL_SELF_CAPTURE=0
WL_FILL=0
UTIL_MIN="${UTIL_MIN:-0}"; UTIL_MAX="${UTIL_MAX:-80}"
DURATION="${DURATION:-300}"
FIO_BIN="${FIO_BIN:-/usr/bin/fio}"
FIO_SIZE_MB="${FIO_SIZE_MB:-30720}"
FIO_BS="${FIO_BS:-4k}"
FIO_NUMJOBS="${FIO_NUMJOBS:-16}"
FIO_RWMIX="${FIO_RWMIX:-50}"
FIO_RAMP="${FIO_RAMP:-10}"
FIO_FILE="$MNT/fio_seq/seqfile"
FIO_PID=""

wl_check_deps() { [[ -x "$FIO_BIN" ]] || die "missing fio"; }

wl_setup() {
  sudo mkdir -p "$MNT/fio_seq"; sudo chown "$USER:$USER" "$MNT/fio_seq"
}

wl_run() {
  local extra=()
  [[ "$FIO_RW" == randrw ]] && extra+=(--rwmixread="$FIO_RWMIX")
  WL_STATUS=0
  if [[ "$FIO_RW" != write ]]; then
    log "[fio] prefill ${FIO_SIZE_MB} MiB"
    $SCOPE $NUMACTL "$FIO_BIN" --name=prefill --filename="$FIO_FILE" --rw=write --bs=1M \
      --size="${FIO_SIZE_MB}M" --ioengine=libaio --direct=1 --iodepth=64 --end_fsync=1 \
      --output-format=json --output="$CASE_DIR/fio_prefill.json" > /dev/null 2>&1 &
    FIO_PID=$!; wait "$FIO_PID" || WL_STATUS=$?; FIO_PID=""
    ((WL_STATUS == 0)) || return 0
    sudo sync
    dm_snapshot measure
    cat "/sys/block/$DEVNAME/stat" > "$CASE_DIR/measure_block_stat.txt"
  fi
  log "[fio] $FIO_RW ${DURATION}s, $FIO_NUMJOBS psync jobs, fdatasync=1"
  $SCOPE $NUMACTL "$FIO_BIN" --name=bench --filename="$FIO_FILE" \
    --rw="$FIO_RW" "${extra[@]}" --bs="$FIO_BS" --size="${FIO_SIZE_MB}M" \
    --ioengine=psync --direct=0 --fdatasync=1 --numjobs="$FIO_NUMJOBS" --thread \
    --time_based --runtime="$DURATION" --ramp_time="$FIO_RAMP" --group_reporting \
    --percentile_list=50:90:95:99:99.9:99.99 \
    --output-format=json --output="$CASE_DIR/fio.json" > "$CASE_DIR/fio.log" 2>&1 &
  FIO_PID=$!; wait "$FIO_PID" || WL_STATUS=$?; FIO_PID=""
}

wl_ok() { python3 -c "import json,sys; json.load(open('$CASE_DIR/fio.json'))['jobs'][0]['write']" 2>/dev/null; }
# only ever the fio this plugin started: $! is the sudo of $SCOPE, which relays the signal to fio - never by name
wl_teardown() { [[ -n "${FIO_PID:-}" ]] && { kill_own "$FIO_PID"; wait "$FIO_PID" 2>/dev/null; FIO_PID=""; }; true; }

wl_config_lines() {
  printf 'fio_bin=%s\nfio_version=%s\nfio_size_mb=%s\nfio_bs=%s\nfio_numjobs=%s\nfio_rw=%s\nfio_rwmix=%s\n' \
    "$FIO_BIN" "$("$FIO_BIN" --version)" "$FIO_SIZE_MB" "$FIO_BS" "$FIO_NUMJOBS" "$FIO_RW" "$FIO_RWMIX"
  printf 'fio_engine=psync\nfio_direct=0\nfio_fdatasync=1\nfio_ramp=%s\n' "$FIO_RAMP"
}
