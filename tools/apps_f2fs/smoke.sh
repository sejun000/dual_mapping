#!/bin/bash
# tools/apps/smoke.sh - exercise every workload plugin end-to-end at toy scale on a loop F2FS image, without
# NVMeVirt (no insmod, no device counters). The loop file lives in /mnt/nvme_mounted/dmapp_smoke (never on /).
#   tools/apps/smoke.sh [workload ...]      (default: all 8)
# Toy parameters only shrink sizes/durations/tenant counts; every other knob is the real one.
# Results: /mnt/nvme_mounted/dmapp_smoke/results/<workload>/STATUS + summary.txt
set -uo pipefail
APPS="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
IMG_DIR=/mnt/nvme_mounted/dmapp_smoke
IMG="$IMG_DIR/f2fs.img"
OUT="$IMG_DIR/results"
WLS=("$@"); ((${#WLS[@]})) || WLS=(vmhot1g oltp fileserverfs tpcc ycsba ycsbf fio_randwrite fio_mix)

findmnt -rn /mnt/nvme_mounted >/dev/null || { echo "/mnt/nvme_mounted is not mounted"; exit 1; }
sudo mkdir -p "$IMG_DIR" "$OUT"; sudo chown "$USER" "$IMG_DIR" "$OUT"
toy() {
  case "$1" in
    vmhot1g)      echo "FB_SCALE=0.002 DURATION=15" ;;
    oltp)         echo "FB_SCALE=0.02 DURATION=15" ;;
    fileserverfs) echo "FB_SCALE=0.01 DURATION=15" ;;
    tpcc)         echo "TPCCM_INSTANCES=2 WAREHOUSES=2 TERMINALS=4 DURATION=20 FILL_TARGET_PERCENT=40" ;;
    ycsba|ycsbf)  echo "YCSBM_INSTANCES=2 RECORDCOUNT=20000 THREADS=8 LOAD_PROCS=2 DURATION=15 FILL_TARGET_PERCENT=20" ;;
    fio_*)        echo "FIO_SIZE_MB=512 DURATION=15 FIO_RAMP=2" ;;
  esac
}
placement_monitor() {   # read-only: every 2 s, the cgroup of every workload process of ours (fio: only ours, by cmdline)
  while true; do
    for p in $(pgrep -x 'postgres|java|filebench|fio'); do
      c="$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)"
      [[ "$(cat /proc/$p/comm 2>/dev/null)" == fio && "$c" != *dmapp_smoke* ]] && continue
      cg="$(cut -d: -f3 /proc/$p/cgroup 2>/dev/null)"; cm="$(cat /proc/$p/comm 2>/dev/null)"
      [[ -n "$cg" && -n "$cm" ]] && echo "$(date +%T) $p $cm $cg"   # (empty = exited between pgrep and the read)
    done
    m="$(cat /sys/fs/cgroup/dmapp.slice/memory.max 2>/dev/null)"; [[ -n "$m" ]] && echo "$(date +%T) slice memory.max=$m high=$(cat /sys/fs/cgroup/dmapp.slice/memory.high)"
    sleep 2
  done
}
rc=0
for wl in "${WLS[@]}"; do
  rm -f "$IMG"; truncate -s 8G "$IMG"
  LOOP="$(sudo losetup -f --show "$IMG")" || { echo "losetup failed"; exit 1; }
  echo "=== smoke $wl on $LOOP"
  placement_monitor > "$OUT/$wl.placement" 2>/dev/null & MON=$!
  env LOOP_MODE=1 LOOP_DEV="$LOOP" OUT_DIR="$OUT/$wl" UTIL_CHECK=warn POSTSTOP_OBSERVE=2 CPU_GOVERNOR= \
      $(toy "$wl") "$APPS/run_app.sh" "$wl" page4k > "$OUT/$wl.console" 2>&1 || rc=1
  kill "$MON"; wait "$MON" 2>/dev/null
  n="$(grep -vc 'slice memory' "$OUT/$wl.placement")"; bad="$(grep -v 'slice memory' "$OUT/$wl.placement" | grep -vc 'dmapp.slice')"
  echo "    placement: $n process samples, $bad outside dmapp.slice; $(grep -m1 'slice memory' "$OUT/$wl.placement" | cut -d' ' -f2-)"
  ((bad == 0)) || rc=1
  if fuser /tmp/dmapp_bench.lock >/dev/null 2>&1; then echo "    LEFTOVER process holds the run lock:"; fuser -v /tmp/dmapp_bench.lock; rc=1; fi
  findmnt -rn /mnt/dmapp_smoke >/dev/null && sudo umount /mnt/dmapp_smoke
  sudo losetup -d "$LOOP"
  echo "    -> $(cat "$OUT/$wl/STATUS" 2>/dev/null)  $(grep -E '^throughput=' "$OUT/$wl/summary.txt" 2>/dev/null)"
done
rm -f "$IMG"
exit $rc
