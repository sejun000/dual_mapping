#!/bin/bash
# tools/apps/run_app.sh - one application benchmark on one dual_mapping NVMeVirt FTL policy.
#   tools/apps/run_app.sh <workload> <policy>
#   workloads: vmhot1g oltp fileserverfs tpcc ycsba ycsbf fio_randwrite fio_mix
#   policies : page4k proposed dftl leaftl fast mu   (~/nvmevirt_<policy>/nvmev.ko)
# Flow (splitEval run_bench.sh, conv-adaptive): insmod (memmap_size=44G, dm_params per policy) -> mkfs.f2fs -s 64
# -> mount (study options) at /mnt/dmapp, ipu_policy=1 -> dataset/preload -> ballast fill -> sync+drop_caches ->
# start capture (nvme flush => "[dm flush]" counters, block stat, f2fs status, samplers, bpftrace) -> workload ->
# end capture -> post-stop observation -> umount -> rmmod ("[dm final]").
# Results: results/apps/<workload>/<policy>/ (STATUS = DONE | FAILED:<reason>, summary.txt, summary.json).
# Env: OUT_DIR (case dir), B18 (bytes; computed from a page4k load if unset), DURATION, plus per-plugin knobs.
#   LOOP_MODE=1 LOOP_DEV=/dev/loopN: smoke mode on a loop device backed by /mnt/nvme_mounted/dmapp_smoke/*
#   (no module, no device counters) - used by smoke.sh only.
set -uo pipefail
[[ $# -eq 2 ]] || { sed -n 2,13p "$0"; exit 2; }
WL="$1"; POL="$2"
if [[ "${LOOP_MODE:-0}" == 1 ]]; then MNT="${MNT:-/mnt/dmapp_smoke}"; else MNT=/mnt/dmapp; fi
. "$(dirname "$(readlink -f "$0")")/common.sh"

case "$WL" in
  vmhot1g)       PLUGIN=filebench; export FB_PROFILE=varmail-hot1g ;;
  oltp)          PLUGIN=filebench; export FB_PROFILE=oltp ;;
  fileserverfs)  PLUGIN=filebench; export FB_PROFILE=fileserverfs ;;
  tpcc)          PLUGIN=tpcc ;;
  ycsba)         PLUGIN=ycsb; export YCSBM_WORKLOAD=workloada ;;
  ycsbf)         PLUGIN=ycsb; export YCSBM_WORKLOAD=workloadf ;;
  fio_randwrite) PLUGIN=fio;  export FIO_RW=randwrite ;;
  fio_mix)       PLUGIN=fio;  export FIO_RW=randrw ;;
  *) die "unknown workload $WL" ;;
esac
PARAMS_RAW="$(policy_params "$POL")" || die "unknown policy $POL"

CASE_DIR="${OUT_DIR:-$RESULTS_ROOT/$WL/$POL}"
if [[ -e "$CASE_DIR" ]]; then mv "$CASE_DIR" "$CASE_DIR.old.$(date -u +%Y%m%dT%H%M%SZ)"; fi
mkdir -p "$CASE_DIR"
exec > >(tee -a "$CASE_DIR/run.log") 2>&1
RUN_ID="${WL}_${POL}_$(date -u +%Y%m%dT%H%M%SZ)"
log "=== $RUN_ID -> $CASE_DIR"
echo "RUNNING" > "$CASE_DIR/STATUS"

exec 9>/tmp/dmapp_bench.lock
flock -n 9 || die "another tools/apps run holds /tmp/dmapp_bench.lock"

. "$APPS_DIR/wl/$PLUGIN.sh"
WL_STATUS=99
KSTART="$(awk '{print $1}' /proc/uptime)"

finish() {   # always: stop everything we started, unmount, unload, record status
  local rc=$?
  trap - EXIT INT TERM
  stop_samplers
  declare -F wl_teardown >/dev/null && wl_teardown
  memcg_stop
  if findmnt -rn "$MNT" >/dev/null 2>&1; then
    sync; sudo umount "$MNT" || { sleep 5; sudo umount "$MNT"; } || log "WARNING: umount $MNT failed"
  fi
  if [[ "${LOOP_MODE:-0}" != 1 && "$NVMEV_LOADED_BY_US" == 1 ]]; then
    nvmev_rmmod || log "WARNING: rmmod failed"
    sudo dmesg | grep '\[dm final\]' | tail -1 > "$CASE_DIR/dm_final.txt"
  fi
  sudo dmesg | awk -v t0="$KSTART" '{ s = $0; if (sub(/^\[ */, "", s) && s + 0 >= t0) print }' \
    > "$CASE_DIR/dmesg.txt" 2>/dev/null || true
  if grep -qE 'kernel BUG|Oops:|soft lockup|hung_task|blocked for more than|WARNING:.*nvmev|Call Trace' "$CASE_DIR/dmesg.txt"; then
    echo "kernel_trouble=1" >> "$CASE_DIR/warnings.txt"
  fi
  if [[ "$(cat "$CASE_DIR/STATUS")" == RUNNING ]]; then echo "FAILED:interrupted(rc=$rc)" > "$CASE_DIR/STATUS"; fi
  log "=== $RUN_ID status $(cat "$CASE_DIR/STATUS")"
  [[ "$(cat "$CASE_DIR/STATUS")" == DONE ]]; exit $?
}
fail() { echo "FAILED:$1" > "$CASE_DIR/STATUS"; log "[FATAL] $1"; exit 1; }
trap finish EXIT
trap 'fail signal' INT TERM

wl_check_deps

# ------------------------------------------------------------- device
if [[ "${LOOP_MODE:-0}" == 1 ]]; then
  DEV="${LOOP_DEV:?LOOP_DEV must be set in loop mode}"
  PARAMS="(loop mode)"
else
  PARAMS="$PARAMS_RAW"
  if [[ "$PARAMS" == *B18* ]]; then
    [[ -n "${B18:-}" ]] || compute_b18
    PARAMS="${PARAMS//B18/$B18}"
  fi
  nvmev_insmod "$POL" "$PARAMS"
  dm_policy_line > "$CASE_DIR/dm_policy.txt"
  grep -q "params=\"$PARAMS\"" "$CASE_DIR/dm_policy.txt" || fail "dmesg policy line does not show params '$PARAMS'"
  DEV="$(nvmev_find_dev)" || fail "no NVMeVirt namespace"
fi
assert_target_dev "$DEV"
DEVNAME="$(basename "$DEV")"
log "target $DEV ($(lsblk -dn -o MODEL,SIZE "$DEV" | tr -s ' '))"
setup_host_knobs
setup_memcg
mkfs_and_mount
log "mounted $DEV at $MNT, f2fs user segments $F2FS_USER_SEGS (study $STUDY_USER_SEGS)"

{
  printf 'run_id=%s\nworkload=%s\nwl_kind=%s\npolicy=%s\ndm_params=%s\nb18=%s\n' "$RUN_ID" "$WL" "$WL_KIND" "$POL" "$PARAMS" "${B18:-}"
  printf 'kernel=%s\ndevice=%s\nmodel=%s\ndevice_bytes=%s\n' "$(uname -r)" "$DEV" \
    "$(lsblk -dn -o MODEL "$DEV" | sed 's/ *$//')" "$(sudo blockdev --getsize64 "$DEV")"
  if [[ "${LOOP_MODE:-0}" != 1 ]]; then
    printf 'nvmev_ko=%s\nnvmev_args=memmap_start=%s memmap_size=%s cpus=%s\n' "$HOME/nvmevirt_$POL/nvmev.ko" \
      "$NVMEV_MEMMAP_START" "$NVMEV_MEMMAP_SIZE" "$NVMEV_CPUS"
    printf 'nvmev_src_commit=%s\n' "$(git -C "$HOME/nvmevirt_$POL" log -1 --format='%h %s' 2>/dev/null)"
    printf 'nvmev_src_dirty=%s\n' "$(git -C "$HOME/nvmevirt_$POL" status --porcelain 2>/dev/null | wc -l)"
    sha256sum "$HOME/nvmevirt_$POL/nvmev.ko"
    printf 'dm_policy_line=%s\n' "$(cat "$CASE_DIR/dm_policy.txt")"
  fi
  printf 'mount_point=%s\nmount_options=%s\nmkfs=%s\nf2fs_segs_per_sec=%s\n' "$MNT" "$MOUNT_OPTIONS" \
    "$(mkfs.f2fs -V 2>&1 | head -1)" "$F2FS_SEGS_PER_SEC"
  printf 'ipu_policy=%s\nmin_ssr_sections=%s\n' "$(cat "$SYSFS_DIR/ipu_policy")" "$(cat "$SYSFS_DIR/min_ssr_sections")"
  printf 'f2fs_main=%s\nf2fs_user_segs=%s\n' "$(f2fs_status | grep -m1 -o '\[MAIN: [^]]*\]')" "$F2FS_USER_SEGS"
  printf 'numa_node=%s\nwl_cpus=%s\nnvmev_cpus=%s\n' "$NUMA_NODE" "$WL_CPUS" "$NVMEV_CPUS"
  printf 'memcg_slice=%s\nmemcg_pct=%s\nmemcg_device_bytes=%s\nmemcg_max_mib=%s\nmemcg_high_mib=%s\nmemcg_max_bytes=%s\nmemcg_high_bytes=%s\n' \
    "$MEMCG_SLICE" "$MEMCG_PCT" "$MEMCG_DEV_BYTES" "$MEMCG_MAX_MIB" "$MEMCG_HIGH_MIB" \
    "$(systemctl show "$MEMCG_SLICE" -p MemoryMax --value)" "$(systemctl show "$MEMCG_SLICE" -p MemoryHigh --value)"
  printf 'fill_target_percent=%s\nutil_window=%s-%s\nduration=%s\n' "$WL_FILL" "$UTIL_MIN" "$UTIL_MAX" "$DURATION"
  cat "$CASE_DIR/host_knobs.txt"
  wl_config_lines
} > "$CASE_DIR/experiment_config.txt" 2>&1

# ------------------------------------------------------------- dataset, fill, measurement
wl_setup || fail "workload setup"
fill_ballast "$WL_FILL"
declare -F wl_post_ballast >/dev/null && { wl_post_ballast || fail "post-ballast"; }
sudo sync
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
if [[ "$WL_SELF_CAPTURE" != 1 ]]; then capture_start || fail "start utilisation outside window"; fi
wl_run
if [[ "$WL_STATUS" == 2 ]]; then fail "start utilisation outside window"; fi
capture_end
k="$(sed -n 's/^memory.events.oom_kill=//p' "$CASE_DIR/memcg_end.txt")"
[[ -n "$k" && "$k" != 0 ]] && { echo "memcg_oom_kill=$k" >> "$CASE_DIR/warnings.txt"; log "WARNING: $k OOM kill(s) in $MEMCG_SLICE"; }
sudo df -B1 "$MNT" > "$CASE_DIR/end_df.txt"
wl_ok || fail "workload produced no result (status $WL_STATUS)"
((WL_STATUS == 0)) || echo "workload_exit_status=$WL_STATUS" >> "$CASE_DIR/warnings.txt"

# post-stop observation (background GC/checkpoint trailing the run), as in the study
wl_teardown
sleep "$POSTSTOP_OBSERVE"
f2fs_status > "$CASE_DIR/poststop_f2fs_status.txt"

echo DONE > "$CASE_DIR/STATUS"
python3 "$APPS_DIR/summarize.py" case "$CASE_DIR" > "$CASE_DIR/summary.txt" 2> "$CASE_DIR/summarize.err" ||
  log "WARNING: summarize failed (see summarize.err)"
cat "$CASE_DIR/summary.txt"
