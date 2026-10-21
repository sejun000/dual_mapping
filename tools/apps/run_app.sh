#!/bin/bash
# tools/apps/run_app.sh - one application benchmark on one dual_mapping NVMeVirt FTL policy.
#   tools/apps/run_app.sh <workload> <policy>
#   workloads: vmhot1g oltp fileserverfs tpcc ycsba ycsbf ycsbb ycsbc fio_randwrite fio_mix  (ycsbb 95:5, ycsbc read-only)
#   policies : page4k proposed dftl leaftl fast mu   (~/nvmevirt_<policy>/nvmev.ko)
# Flow (splitEval run_bench.sh, conv-adaptive workloads; ext4 since 2026-10-05): insmod (memmap_size=44G,
# dm_flush_dump=0, dm_params per policy) -> mkfs.ext4 -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 -> mount
# noatime,data=ordered at /mnt/dmapp -> dataset/preload -> df-based ballast fill -> sync+drop_caches -> start capture
# (dm_dump sysfs trigger => "[dm flush]" counters, block stat, df/ext4 sysfs, samplers, bpftrace) -> workload ->
# end capture -> post-stop observation -> umount -> rmmod ("[dm final]").
# Results: results/apps_ext4/<workload>/<policy>/ (STATUS = DONE | FAILED:<reason>, summary.txt, summary.json).
# Env: OUT_DIR (case dir), B18 (bytes; computed from a page4k load if unset), DURATION, plus per-plugin knobs.
#   LOOP_MODE=1 LOOP_DEV=/dev/loopN: smoke mode on a loop device backed by /mnt/nvme_mounted/dmapp_smoke/*
#   (no module, no device counters) - used by smoke.sh only.
#   CAPTURE_BLKTRACE=1 [BLKTRACE_DIR=dir]: also record the block-level request stream with blktrace (from just before
#   mkfs to the end of the measurement, markers at measurement start/end) - see tools/apps/blkcap.sh, tools/apps/blk2csv.
set -uo pipefail
[[ $# -eq 2 ]] || { sed -n 2,15p "$0"; exit 2; }
WL="$1"; POL="$2"
if [[ "${LOOP_MODE:-0}" == 1 ]]; then MNT="${MNT:-/mnt/dmapp_smoke}"; else MNT=/mnt/dmapp; fi
. "$(dirname "$(readlink -f "$0")")/common.sh"

# <workload>_t<N>: same workload with N client threads in total (YCSB THREADS / TPC-C TERMINALS; splitEval used 320)
BASE_WL="$WL"
if [[ "$WL" =~ ^(.+)_t([0-9]+)$ ]]; then BASE_WL="${BASH_REMATCH[1]}"; export THREADS="${BASH_REMATCH[2]}" TERMINALS="${BASH_REMATCH[2]}"; fi
case "$BASE_WL" in
  vmhot1g)       PLUGIN=filebench; export FB_PROFILE=varmail-hot1g ;;
  oltp)          PLUGIN=filebench; export FB_PROFILE=oltp ;;
  fileserverfs)  PLUGIN=filebench; export FB_PROFILE=fileserverfs ;;
  tpcc)          PLUGIN=tpcc ;;
  ycsba)         PLUGIN=ycsb; export YCSBM_WORKLOAD=workloada ;;
  ycsbf)         PLUGIN=ycsb; export YCSBM_WORKLOAD=workloadf ;;
  ycsbb)         PLUGIN=ycsb; export YCSBM_WORKLOAD=workloadb; export YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=0.95 updateproportion=0.05}" ;;
  ycsbc)         PLUGIN=ycsb; export YCSBM_WORKLOAD=workloadc; export YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=1.0 updateproportion=0}" ;;
  # *_d: same workloads with PostgreSQL 16 stock settings instead of splitEval's small-write pressure profiles
  ycsba_d|ycsbf_d|ycsbb_d|ycsbc_d|ycsbfstd_d|ycsbd_d|ycsbe_d)
                 PLUGIN=ycsb; export YCSBM_WORKLOAD=workload${BASE_WL:4:1} POSTGRES_PROFILE=pg16-default
                 case "$BASE_WL" in ycsbb_d) export YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=0.95 updateproportion=0.05}" ;;
                               ycsbc_d) export YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=1.0 updateproportion=0}" ;;
                               # standard YCSB-F (splitEval forced updateproportion=0.5 on F too -> read/update/RMW 40/40/20)
                               # D/E: standard proportions (explicit, since wl/ycsb.sh injects read/update 0.5/0.5 when empty)
                               ycsbd_d) export YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=0.95 updateproportion=0 scanproportion=0 insertproportion=0.05}" ;;
                               ycsbe_d) export YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=0 updateproportion=0 scanproportion=0.95 insertproportion=0.05}" ;;
                               ycsbfstd_d) export YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=0.5 updateproportion=0 readmodifywriteproportion=0.5}" ;; esac ;;
  tpcc_d)        PLUGIN=tpcc; export SHARED_BUFFERS=128MB CHECKPOINT_TIMEOUT=5min BGWRITER_DELAY=200ms \
                   BGWRITER_LRU_MAXPAGES=100 BGWRITER_LRU_MULTIPLIER=2.0 MAX_WAL_SIZE=512MB MIN_WAL_SIZE=80MB ;;
  fio_randwrite_d) PLUGIN=fio; export FIO_RW=randwrite FIO_FDATASYNC=0 ;;   # fio default: no per-write fdatasync
  fio_mix_d)     PLUGIN=fio;  export FIO_RW=randrw FIO_FDATASYNC=0 ;;
  fio_mix70_d)   PLUGIN=fio;  export FIO_RW=randrw FIO_RWMIX=70 FIO_FDATASYNC=0 ;;   # 70% reads
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
# opt-in block trace: defines blkcap_start/blkcap_stop and wraps capture_start/capture_end/dm_snapshot with markers
if [[ "${CAPTURE_BLKTRACE:-0}" == 1 ]]; then . "$APPS_DIR/blkcap.sh"; fi
WL_STATUS=99
KSTART="$(awk '{print $1}' /proc/uptime)"

finish() {   # always: stop everything we started, unmount, unload, record status
  local rc=$?
  trap - EXIT INT TERM
  if declare -F blkcap_stop >/dev/null; then blkcap_stop; fi   # CAPTURE_BLKTRACE: never leave blktrace running
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
  if [[ "$PARAMS" == *B18* || "$PARAMS" == *B10* || "$PARAMS" == *B12* || "$PARAMS" == *B25* ]]; then
    [[ -n "${B18:-}" ]] || compute_b18
    LPG=$((B18 * 10000 / 7500))                              # B18 = L x 4 x 0.1875 -> L (exact for the 44G device)
    ((LPG * 7500 / 10000 == B18)) || die "B18=$B18 is not L x 4 x 0.1875 for an integer L"
    PARAMS="${PARAMS//B18/$B18}"
    PARAMS="${PARAMS//B10/$((LPG * 4 * 1000 / 10000))}"     # 10% of the page map
    PARAMS="${PARAMS//B25/$((LPG * 4 * 2500 / 10000))}"     # 25% of the page map
    PARAMS="${PARAMS//B12/$((LPG * 4 * 1250 / 10000))}"     # 12.5% (1/8) of the page map
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
# CAPTURE_BLKTRACE: start the trace right BEFORE mkfs (not after it) so the mkfs writes are in the trace and the
# never-written LBA space can be identified from it; everything up to marker dm_measure_start is warm-up.
# PREFILL_SEQ=1 ("aged device", SNIA-PTS-style precondition): write the whole NVMeVirt namespace once sequentially
# (128K, QD32, direct) before mkfs, so every LBA is mapped as on an SSD used without TRIM. Not part of the measurement.
prefill_seq() {
  local dev="$1" name size rootdisk t0 t1
  name="$(basename "$dev")"
  assert_target_dev "$dev"                                                   # NVMe namespace, CSL_Virt model, no holders
  [[ "$name" =~ ^nvme[0-9]+n[0-9]+$ ]] || die "prefill: refusing $dev (not an NVMe namespace)"
  for d in $PROTECTED_DISKS sda; do [[ "$name" == "$d" ]] && die "prefill: refusing protected disk $dev"; done
  rootdisk="$(lsblk -no PKNAME "$(findmnt -n -o SOURCE /)" 2>/dev/null | head -1)"
  [[ -n "$rootdisk" && "$name" != "$rootdisk" ]] || die "prefill: refusing $dev (boot disk '$rootdisk' or unknown)"
  [[ "$(lsblk -dn -o MODEL "$dev" | sed 's/[[:space:]]*$//')" == ${NVMEV_MODEL_PREFIX}* ]] || die "prefill: $dev model is not ${NVMEV_MODEL_PREFIX}*"
  [[ -z "$(lsblk -n -o MOUNTPOINT "$dev" | tr -d '[:space:]')" ]] || die "prefill: $dev or a partition of it is mounted"
  size="$(sudo blockdev --getsize64 "$dev")"
  (( size > 0 && size < (100 << 30) )) || die "prefill: $dev size $size B is not an NVMeVirt-sized device (<100 GiB)"
  log "[prefill] sequential 128K write of the whole $dev ($size B) before mkfs"
  t0=$(date +%s)
  sudo fio --name=prefill_seq --filename="$dev" --direct=1 --ioengine=libaio --rw=write --bs=128k --iodepth=32 \
       --size=100% --output-format=json --output="$CASE_DIR/prefill_seq.json" >/dev/null || die "prefill fio failed"
  t1=$(date +%s)
  log "[prefill] done in $((t1 - t0)) s"
  printf 'prefill_seq=1\nprefill_seq_s=%s\nprefill_seq_bytes=%s\n' "$((t1 - t0))" "$size" > "$CASE_DIR/prefill_seq.txt"
}
if [[ "${PREFILL_SEQ:-0}" == 1 ]]; then
  if [[ "${LOOP_MODE:-0}" == 1 ]]; then log "[prefill] skipped in loop mode"; else prefill_seq "$DEV"; fi
fi
if [[ "${CAPTURE_BLKTRACE:-0}" == 1 ]]; then blkcap_start; fi
mkfs_and_mount
log "mounted $DEV at $MNT (ext4, capacity $FS_CAPACITY B)"

{
  printf 'run_id=%s\nworkload=%s\nwl_kind=%s\npolicy=%s\ndm_params=%s\nb18=%s\n' "$RUN_ID" "$WL" "$WL_KIND" "$POL" "$PARAMS" "${B18:-}"
  printf 'kernel=%s\ndevice=%s\nmodel=%s\ndevice_bytes=%s\n' "$(uname -r)" "$DEV" \
    "$(lsblk -dn -o MODEL "$DEV" | sed 's/ *$//')" "$(sudo blockdev --getsize64 "$DEV")"
  if [[ "${LOOP_MODE:-0}" != 1 ]]; then
    printf 'nvmev_ko=%s\nnvmev_args=memmap_start=%s memmap_size=%s cpus=%s\n' "$HOME/nvmevirt_${POL%%_*}/nvmev.ko" \
      "$NVMEV_MEMMAP_START" "$NVMEV_MEMMAP_SIZE" "$NVMEV_CPUS"
    printf 'nvmev_src_commit=%s\n' "$(git -C "$HOME/nvmevirt_${POL%%_*}" log -1 --format='%h %s' 2>/dev/null)"
    printf 'nvmev_src_dirty=%s\n' "$(git -C "$HOME/nvmevirt_${POL%%_*}" status --porcelain 2>/dev/null | wc -l)"
    sha256sum "$HOME/nvmevirt_${POL%%_*}/nvmev.ko"
    printf 'dm_policy_line=%s\n' "$(cat "$CASE_DIR/dm_policy.txt")"
  fi
  printf 'fs=ext4\nmount_point=%s\nmount_options=%s\nmkfs=mkfs.ext4 %s (%s)\n' "$MNT" "$MOUNT_OPTIONS" \
    "$EXT4_MKFS_OPTS" "$(mkfs.ext4 -V 2>&1 | head -1)"
  grep -E '^(Filesystem features|Block count|Reserved block count|Block size|Inode count|Inode size|Journal size|Flex block group size):' \
    "$CASE_DIR/dumpe2fs.txt" | sed -E 's/^([^:]*):[[:space:]]+/\1=/; s/^([^=]*)/\L\1/; s/^([^=]*)/ext4_\1/; :a; s/^([^=]*) /\1_/; ta' 
  printf 'fs_capacity_bytes=%s\n' "$FS_CAPACITY"
  printf 'numa_node=%s\nwl_cpus=%s\nnvmev_cpus=%s\n' "$NUMA_NODE" "$WL_CPUS" "$NVMEV_CPUS"
  printf 'memcg_slice=%s\nmemcg_pct=%s\nmemcg_device_bytes=%s\nmemcg_max_mib=%s\nmemcg_high_mib=%s\nmemcg_max_bytes=%s\nmemcg_high_bytes=%s\n' \
    "$MEMCG_SLICE" "$MEMCG_PCT" "$MEMCG_DEV_BYTES" "$MEMCG_MAX_MIB" "$MEMCG_HIGH_MIB" \
    "$(systemctl show "$MEMCG_SLICE" -p MemoryMax --value)" "$(systemctl show "$MEMCG_SLICE" -p MemoryHigh --value)"
  printf 'fill_target_percent=%s\nutil_window=%s-%s\nduration=%s\n' "$WL_FILL" "$UTIL_MIN" "$UTIL_MAX" "$DURATION"
  cat "$CASE_DIR/host_knobs.txt"
  if [[ "${CAPTURE_BLKTRACE:-0}" == 1 ]]; then printf 'blktrace_dir=%s\n' "$BLKCAP_DIR"; fi
  wl_config_lines
} > "$CASE_DIR/experiment_config.txt" 2>&1

# ------------------------------------------------------------- dataset, fill, measurement
wl_setup || fail "workload setup"
fill_ballast "$WL_FILL" "${WL_FILL_PENDING:-0}"
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

# post-stop observation (writeback/journal commits trailing the run), as in the study
wl_teardown
sleep "$POSTSTOP_OBSERVE"
for f in lifetime_write_kbytes session_write_kbytes; do echo "$f=$(cat "$SYSFS_DIR/$f")"; done > "$CASE_DIR/poststop_ext4_sysfs.txt"
cat "/sys/block/$DEVNAME/stat" > "$CASE_DIR/poststop_block_stat.txt"

echo DONE > "$CASE_DIR/STATUS"
python3 "$APPS_DIR/summarize.py" case "$CASE_DIR" > "$CASE_DIR/summary.txt" 2> "$CASE_DIR/summarize.err" ||
  log "WARNING: summarize failed (see summarize.err)"
cat "$CASE_DIR/summary.txt"
