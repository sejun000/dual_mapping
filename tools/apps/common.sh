# tools/apps/common.sh - shared chassis for the application benchmarks on the dual_mapping NVMeVirt FTLs.
# Port of splitEval's scripts/runner_lib.sh (conv-adaptive workloads) to: ext4 on stock 6.8 (user decision
# 2026-10-05; the F2FS version of these scripts is kept in tools/apps_f2fs, results in results/apps), one NVMeVirt
# namespace, pluggable dm_ftl policies in ~/nvmevirt_<policy> loaded with dm_flush_dump=0.
# Sourced by run_app.sh; not a standalone script.

APPS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$APPS_DIR/../.." && pwd)"
RESULTS_ROOT="${RESULTS_ROOT:-$REPO_DIR/results/apps_ext4}"

# ---------------------------------------------------------------- device / module
NVMEV_MEMMAP_START="${NVMEV_MEMMAP_START:-256G}"
NVMEV_MEMMAP_SIZE="${NVMEV_MEMMAP_SIZE:-44G}"         # splitEval BASELINE_SIZE (variables.sh)
NVMEV_CPUS="${NVMEV_CPUS:-94,95}"                     # as tools/nvmev_cycle.sh
NVMEV_MODEL_PREFIX="CSL_Virt"
PROTECTED_DISKS="${PROTECTED_DISKS:-nvme0n1 nvme1n1 nvme2n1}"   # real data disks: never a target, whatever their model says
SMOKE_IMG_DIR="/mnt/nvme_mounted/dmapp_smoke"         # loop-mode backing files must live here

# dm_params per policy, exactly as queued through tools/nvmev_cycle.sh (results/nvmevirt/cycle_*.log).
# B18 = 18.75% of the page map in bytes (L x 4 x 0.1875), L read from the loaded page4k device.
policy_params() {
  case "$1" in
    # 2026-10-13 final matrix (results/final_b12): every policy at 12.5% (1/8) mapping DRAM.
    #   fast  : cache_bytes caps the random log blocks so the log map fits the budget (fast_mem <= B12)
    #   iurmw : 32 KiB IU (U=8) -> 4 B per 32 KiB = exactly 1/8 of the page map
    # (earlier results, e.g. results/apps_aged, used fast without a cap and iurmw at U=16)
    page4k) echo "" ;;
    fast) echo "cache_bytes=B12" ;;
    iurmw) echo "U=8" ;;
    proposed) echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500" ;;
    dftl) echo "cache_bytes=B18,dftl_tpbuf=1" ;;
    leaftl|mu) echo "cache_bytes=B18" ;;
    # sensitivity aliases (results/nvmevirt/q_sens.sh): <policy>_<variant> loads ~/nvmevirt_<policy>/nvmev.ko with
    # other dm_params. B10 / B25 = 10% / 25% of the page map in bytes (run_app.sh derives them from B18).
    proposed_b10) echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=100000" ;;
    proposed_b25) echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=250000" ;;
    proposed_u8)  echo "U=8,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500" ;;
    proposed_u32) echo "U=32,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500" ;;
    # 12.5% (1/8) budget = DRAM of a 32 KiB-IU SSD
    proposed_b12) echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125000" ;;
    dftl_b12)     echo "cache_bytes=B12,dftl_tpbuf=1" ;;
    # +f: with the 2026-10-13 fixes onefront_fix=1 pool_cap=4 (fine-pool saturation guard; no 18.75% regressions)
    proposed_b12f) echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125000,onefront_fix=1,pool_cap=4" ;;
    # +s: spill=2 (2026-10-19): full-bucket evictions spill fine maps to flash translation pages instead of folding
    proposed_b12s) echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2" ;;
    # +r: spill=2 + clean read-promotion (2026-10-19, results/spill_rp): a host read of a spilled IU brings its pointers
    # back into the table and keeps its FTP image valid, so re-evicting it costs no FTP program. Needs the rebuilt module.
    proposed_b12r) echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1" ;;
    proposed_f)    echo "U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500,onefront_fix=1,pool_cap=4" ;;
    leaftl_b12)   echo "cache_bytes=B12,leaftl_batch=128" ;;   # sort batch 512 KiB; staged pages hold write-buffer space until programmed (glue dm_pend)
    mu_b12)       echo "cache_bytes=B12" ;;
    dftl_b10)     echo "cache_bytes=B10,dftl_tpbuf=1" ;;
    dftl_b25)     echo "cache_bytes=B25,dftl_tpbuf=1" ;;
    leaftl_b10)   echo "cache_bytes=B10" ;;
    leaftl_b25)   echo "cache_bytes=B25" ;;
    *) return 1 ;;
  esac
}
ALL_POLICIES=(page4k proposed dftl leaftl fast mu)

# ---------------------------------------------------------------- host placement
# NUMA as the study (numa_node=1). Memory: ONE cgroup for the whole workload (user decision 2026-10-04): the transient
# systemd slice dmapp.slice with MemoryMax = MEMCG_PCT% of the device's logical size (study: 8G on a ~40 GB device)
# and MemoryHigh = 7/8 of it (study 7G/8G), both rounded down to MiB and computed at run time (setup_memcg).
# Every workload process is in it: systemd-run units get --slice=dmapp.slice (filebench, the TPC-C PostgreSQL
# units, which keep their own nested 1G/896M limits), directly started processes run through $SCOPE
# (systemd-run --scope --slice=dmapp.slice: fio, YCSB's pg_ctl/postgres and JVMs, BenchBase JVMs).
NUMA_NODE="${NUMA_NODE:-1}"
MEMCG_SLICE=dmapp.slice
MEMCG_PCT="${MEMCG_PCT:-20}"
MEMCG_DIR="/sys/fs/cgroup/$MEMCG_SLICE"
SCOPE="sudo systemd-run --quiet --scope --slice=$MEMCG_SLICE --uid=$(id -u) --gid=$(id -g) --"
# workload CPUs = the study's NUMA node minus the NVMeVirt dispatcher/IO-worker CPUs (the study kept the
# workload node and the NVMeVirt CPUs disjoint; here CPU 95 is on node 1, so it is carved out).
wl_cpu_list() {
  python3 - "$NUMA_NODE" "$NVMEV_CPUS" <<'PY'
import sys
def expand(s):
    out = []
    for part in s.split(','):
        if not part: continue
        if '-' in part:
            a, b = part.split('-'); out += range(int(a), int(b) + 1)
        else: out.append(int(part))
    return out
node = open(f"/sys/devices/system/node/node{sys.argv[1]}/cpulist").read().strip()
excl = set(expand(sys.argv[2]))
print(','.join(str(c) for c in expand(node) if c not in excl))
PY
}
WL_CPUS="${WL_CPUS:-$(wl_cpu_list)}"
NUMACTL="/usr/bin/numactl --physcpubind=$WL_CPUS --membind=$NUMA_NODE"

# ---------------------------------------------------------------- file system: ext4
# No discard anywhere (our FTLs have no TRIM): mkfs nodiscard, no 'discard' mount option. Inode tables and the
# journal are fully initialised by mkfs so there is no ext4lazyinit background I/O during a run.
EXT4_MKFS_OPTS="${EXT4_MKFS_OPTS:--F -E nodiscard,lazy_itable_init=0,lazy_journal_init=0}"
EXT4_MOUNT_OPTS="${EXT4_MOUNT_OPTS:-noatime,data=ordered}"
# Utilisation = df Use% = used / (used + avail) (excludes ext4's 5% root reserve), the number targets refer to.
NVME_IO_TIMEOUT="${NVME_IO_TIMEOUT:-255}"             # runtime nvme_core params (study: max_retries=255)
NVME_MAX_RETRIES="${NVME_MAX_RETRIES:-255}"
CPU_GOVERNOR="${CPU_GOVERNOR-performance}"           # study: cpu_governor=performance ("" = leave alone)
UTIL_CHECK="${UTIL_CHECK:-abort}"                     # abort|warn when start utilisation is outside the window
POSTSTOP_OBSERVE="${POSTSTOP_OBSERVE:-45}"
DISK_SAMPLE_SECONDS=1
FS_SAMPLE_SECONDS=2
BLKLAT="${BLKLAT:-1}"                                 # bpftrace block-layer latency histograms + app write bytes

log() { echo "[$(date -u +%H:%M:%S)] $*"; }
die() { echo "[FATAL] $*" >&2; exit 1; }

# ---------------------------------------------------------------- device identification (refuses everything else)
nvmev_find_dev() {   # prints /dev/nvmeXnY of the single CSL_Virt namespace, or fails
  local p m found=()
  for p in /sys/block/nvme*n*; do
    [[ -r "$p/device/model" ]] || continue
    m="$(sed -e 's/[[:space:]]*$//' "$p/device/model")"
    [[ "$m" == ${NVMEV_MODEL_PREFIX}* ]] && found+=("/dev/$(basename "$p")")
  done
  ((${#found[@]} == 1)) || { echo "[FATAL] expected exactly one ${NVMEV_MODEL_PREFIX}* namespace, found: ${found[*]:-none}" >&2; return 1; }
  echo "${found[0]}"
}

assert_target_dev() {   # $1 device; dies unless it is the NVMeVirt namespace (or, in loop mode, our smoke loop)
  local dev="$1" name model d back
  name="$(basename "$dev")"
  for d in $PROTECTED_DISKS; do [[ "$name" == "$d" ]] && die "refusing protected disk $dev"; done
  if [[ "${LOOP_MODE:-0}" == 1 ]]; then
    [[ "$name" =~ ^loop[0-9]+$ ]] || die "loop mode: $dev is not a loop device"
    back="$(losetup -n -O BACK-FILE "$dev" 2>/dev/null | sed 's/[[:space:]]*$//')"
    [[ "$back" == "$SMOKE_IMG_DIR"/* ]] || die "loop mode: $dev backs '$back', not a file under $SMOKE_IMG_DIR"
    return 0
  fi
  [[ "$name" =~ ^nvme[0-9]+n[0-9]+$ ]] || die "refusing $dev: not an NVMe namespace"
  model="$(sed -e 's/[[:space:]]*$//' "/sys/block/$name/device/model" 2>/dev/null)"
  [[ "$model" == ${NVMEV_MODEL_PREFIX}* ]] || die "refusing $dev: model '$model' is not ${NVMEV_MODEL_PREFIX}*"
  model="$(lsblk -dn -o MODEL "$dev" 2>/dev/null | sed 's/[[:space:]]*$//')"
  [[ "$model" == ${NVMEV_MODEL_PREFIX}* ]] || die "refusing $dev: lsblk MODEL '$model' is not ${NVMEV_MODEL_PREFIX}*"
  [[ -z "$(ls /sys/block/$name/holders 2>/dev/null)" ]] || die "refusing $dev: it has holders"
  local tg; tg="$(findmnt -rn -S "$dev" -o TARGET 2>/dev/null)"
  if [[ -n "$tg" && "$tg" != "$MNT" ]]; then die "refusing $dev: mounted at $tg"; fi
}

other_nvmev_users() {   # non-empty if someone else's NVMeVirt experiment is running
  # anchored on the interpreter so a waiter like  bash -c 'until ! pgrep -f nvmev_cycle.sh ...; tools/apps/run_all.sh'
  # does not count as a user
  pgrep -fa '^(/usr)?(/bin/)?(ba)?sh [^ ]*nvmev_(cycle|randwrite|mixed)\.sh' 2>/dev/null || true
}

# ---------------------------------------------------------------- module load / unload
NVMEV_LOADED_BY_US=0
nvmev_loaded() { grep -q '^nvmev ' /proc/modules; }   # (no pipe: 'lsmod | grep -q' is unreliable under pipefail)
port_in_use() { [[ "$(ss -Hltn "sport = :$1")" == *LISTEN* ]]; }
nvmev_insmod() {   # $1 policy folder, $2 dm_params (may be empty); waits for the CSL_Virt namespace
  local pol="$1" par="$2" ko="$HOME/nvmevirt_${1%%_*}/nvmev.ko" i   # alias <policy>_<variant> -> ~/nvmevirt_<policy>
  [[ -f "$ko" ]] || die "missing $ko"
  nvmev_loaded && die "nvmev is already loaded (someone else's experiment?) - not touching it"
  [[ -z "$(other_nvmev_users)" ]] || die "an nvmev_cycle/randwrite/mixed run is active - not touching NVMeVirt"
  log "insmod $ko memmap_start=$NVMEV_MEMMAP_START memmap_size=$NVMEV_MEMMAP_SIZE cpus=$NVMEV_CPUS dm_flush_dump=0 dm_params='$par'"
  # dm_flush_dump=0: no counter line per NVMe Flush (fsync-heavy workloads); counters are taken via dm_dump
  sudo insmod "$ko" memmap_start="$NVMEV_MEMMAP_START" memmap_size="$NVMEV_MEMMAP_SIZE" cpus="$NVMEV_CPUS" \
       dm_flush_dump=0 ${par:+dm_params=$par} || die "insmod failed"
  NVMEV_LOADED_BY_US=1
  [[ "$(cat /sys/module/nvmev/parameters/dm_flush_dump 2>/dev/null)" == N ]] || die "dm_flush_dump is not 0"
  [[ -e /sys/module/nvmev/parameters/dm_dump ]] || die "module has no dm_dump trigger (old build?)"
  for i in $(seq 60); do [[ "$(lsblk -dn -o MODEL)" == *"$NVMEV_MODEL_PREFIX"* ]] && return 0; sleep 1; done
  die "no ${NVMEV_MODEL_PREFIX}* namespace appeared"
}

nvmev_rmmod() {
  [[ "$NVMEV_LOADED_BY_US" == 1 ]] || return 0
  findmnt -rn "$MNT" >/dev/null 2>&1 && { sudo umount "$MNT" || return 1; }
  nvmev_loaded && sudo rmmod nvmev
  NVMEV_LOADED_BY_US=0
}

dm_policy_line() { sudo dmesg | grep "dm_ftl policy" | tail -1; }

compute_b18() {   # loads page4k, reads L from dmesg, sets B18 = 18.75% of the 4-byte page map (as nvmev_cycle.sh)
  local L line
  nvmev_insmod page4k ""
  line="$(dm_policy_line)"
  nvmev_rmmod
  [[ "$line" == *"policy=page4k"* ]] || die "unexpected dm_ftl line after page4k load: $line"
  L="$(sed -E 's/.*logical ([0-9]+) pages.*/\1/' <<< "$line")"
  [[ "$L" =~ ^[0-9]+$ ]] || die "could not read logical pages from dmesg"
  B18=$((L * 4 * 1875 / 10000))
  log "B18 = $B18 bytes (page4k L=$L pages)"
}

dm_snapshot() {   # $1 tag: the dm_dump sysfs trigger makes dm_ftl print its counters as a "[dm flush]" line
  local tag="$1" l0 l1 i
  [[ "${LOOP_MODE:-0}" == 1 ]] && { echo "loop mode: no device counters" > "$CASE_DIR/dm_$tag.txt"; return 0; }
  l0="$(sudo dmesg | grep '\[dm flush\]' | tail -1)"
  echo 1 | sudo tee /sys/module/nvmev/parameters/dm_dump >/dev/null || log "WARNING: dm_dump trigger failed"
  for i in $(seq 40); do
    l1="$(sudo dmesg | grep '\[dm flush\]' | tail -1)"; [[ -n "$l1" && "$l1" != "$l0" ]] && break; sleep 0.25
  done
  [[ -n "$l1" && "$l1" != "$l0" ]] || log "WARNING: no new [dm flush] line for $tag"
  { date +%s.%N; echo "$l1"; } > "$CASE_DIR/dm_$tag.txt"
}

# ---------------------------------------------------------------- host knobs
setup_host_knobs() {
  local p q="/sys/block/$DEVNAME/queue"
  {
    echo "nvme_core_io_timeout_before=$(cat /sys/module/nvme_core/parameters/io_timeout)"
    echo "nvme_core_max_retries_before=$(cat /sys/module/nvme_core/parameters/max_retries)"
    echo "cpu_governor_before=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null)"
  } >> "$CASE_DIR/host_knobs.txt"
  if [[ "${LOOP_MODE:-0}" != 1 ]]; then
    (( $(cat /sys/module/nvme_core/parameters/io_timeout) >= NVME_IO_TIMEOUT )) ||
      echo "$NVME_IO_TIMEOUT" | sudo tee /sys/module/nvme_core/parameters/io_timeout >/dev/null
    echo "$NVME_MAX_RETRIES" | sudo tee /sys/module/nvme_core/parameters/max_retries >/dev/null
    echo none | sudo tee "$q/scheduler" >/dev/null       # fs_6.12.sh: using_scheduler=none
  fi
  if [[ -n "$CPU_GOVERNOR" ]]; then
    for p in /sys/devices/system/cpu/cpufreq/policy*; do
      [[ -e "$p/scaling_governor" ]] && echo "$CPU_GOVERNOR" | sudo tee "$p/scaling_governor" >/dev/null
    done
  fi
  {
    for p in scheduler write_cache discard_max_bytes nr_requests max_sectors_kb wbt_lat_usec rotational; do
      echo "queue_$p=$(cat $q/$p 2>/dev/null)"
    done
    echo "nvme_core_io_timeout=$(cat /sys/module/nvme_core/parameters/io_timeout)"
    echo "nvme_core_max_retries=$(cat /sys/module/nvme_core/parameters/max_retries)"
    echo "cpu_governor=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null)"
  } >> "$CASE_DIR/host_knobs.txt"
}

# ---------------------------------------------------------------- ext4
df_util() {   # df Use% as an integer floor: used / (used + avail)
  df -B1 --output=used,avail "$MNT" | tail -1 | awk '{ printf "%d\n", $1 * 100 / ($1 + $2) }'
}
df_capacity() { df -B1 --output=used,avail "$MNT" | tail -1 | awk '{ print $1 + $2 }'; }   # bytes a 100% fill means
jbd2_kthreads() { ps -e -o pid=,comm= | awk -v d="jbd2/$DEVNAME-" 'index($2, d) == 1 { print $1 }'; }

mkfs_and_mount() {
  local pid
  sudo mkdir -p "$MNT"
  findmnt -rn "$MNT" >/dev/null 2>&1 && die "$MNT is already mounted"
  assert_target_dev "$DEV"
  log "mkfs.ext4 $EXT4_MKFS_OPTS $DEV"
  sudo $NUMACTL mkfs.ext4 $EXT4_MKFS_OPTS "$DEV" > "$CASE_DIR/mkfs.log" 2>&1 || die "mkfs.ext4 failed"
  sudo $NUMACTL mount -t ext4 -o "$EXT4_MOUNT_OPTS" "$DEV" "$MNT" || die "mount failed"
  for pid in $(jbd2_kthreads); do   # as the study pinned its FS kthreads: the journal thread on the workload CPUs
    sudo taskset -pc "$WL_CPUS" "$pid" >> "$CASE_DIR/fs_kthreads.txt" 2>&1
  done
  sudo chown "$USER:$USER" "$MNT"
  SYSFS_DIR="/sys/fs/ext4/$DEVNAME"
  MOUNT_OPTIONS="$(findmnt -n -o OPTIONS "$MNT")"
  [[ "$MOUNT_OPTIONS" == *data=ordered* && "$MOUNT_OPTIONS" != *discard* ]] || die "unexpected mount options: $MOUNT_OPTIONS"
  sudo dumpe2fs -h "$DEV" > "$CASE_DIR/dumpe2fs.txt" 2>/dev/null
  FS_CAPACITY="$(df_capacity)"
  log "ext4 capacity (used+avail) $FS_CAPACITY B, util $(df_util)%"
}

fill_ballast() {   # $1 target df Use%, $2 bytes the workload itself will still add (fio's file): dd zero files to
                   # (target - pending), then top up against df until reached (splitEval runner_lib.sh, df-based)
  local target_pct="$1" pending="${2:-0}" cap used need round
  [[ "$target_pct" == 0 ]] && return 0
  sudo sync
  cap="$(df_capacity)"; used="$(df -B1 --output=used "$MNT" | tail -1 | tr -d ' ')"
  need=$((cap * target_pct / 100 - used - pending))
  if ((need <= 0)); then log "[ballast] none: used=$used + pending=$pending already >= ${target_pct}% of $cap"; return 0; fi
  sudo mkdir -p "$MNT/ballast"
  _ballast() {
    local mb="$1" f
    while ((mb > 0)); do
      f=$((mb > 2048 ? 2048 : mb))
      sudo dd if=/dev/zero of="$MNT/ballast/ballast_$BALLAST_I" bs=1M count="$f" conv=fsync status=none
      mb=$((mb - f)); BALLAST_I=$((BALLAST_I + 1))
    done
    sudo sync
  }
  BALLAST_I=0
  log "[ballast] writing $((need >> 20)) MiB to reach ~${target_pct}% (pending $((pending >> 20)) MiB)"
  _ballast $((need >> 20))
  for round in 1 2 3; do
    used="$(df -B1 --output=used "$MNT" | tail -1 | tr -d ' ')"
    need=$((cap * target_pct / 100 - used - pending))
    ((need >= 1048576)) || break
    log "[ballast] df short by $((need >> 20)) MiB; topping up (round $round)"
    _ballast $((need >> 20))
  done
  log "[ballast] df util now $(df_util)%"
}

# ---------------------------------------------------------------- memory cgroup (dmapp.slice)
memcg_procs() {   # "pid comm cgroup" of every process anywhere below the slice
  local f p
  [[ -d "$MEMCG_DIR" ]] || return 0
  while IFS= read -r f; do
    while read -r p; do
      [[ -n "$p" ]] && echo "$p $(cat /proc/$p/comm 2>/dev/null) ${f#/sys/fs/cgroup/}"
    done < "$f"
  done < <(find "$MEMCG_DIR" -name cgroup.procs)
}

setup_memcg() {   # size the common slice from the loaded device; refuse if it still holds processes
  local bytes
  bytes="$(sudo blockdev --getsize64 "$DEV")" || die "blockdev failed"
  [[ -z "$(memcg_procs)" ]] || { memcg_procs >&2; die "$MEMCG_SLICE still has processes from an earlier run"; }
  MEMCG_DEV_BYTES="$bytes"
  MEMCG_MAX_MIB=$((bytes * MEMCG_PCT / 100 / 1048576))
  MEMCG_HIGH_MIB=$((MEMCG_MAX_MIB * 7 / 8))
  sudo systemctl stop "$MEMCG_SLICE" 2>/dev/null || true      # empty -> fresh cgroup, fresh memory.peak/events
  sudo systemctl set-property --runtime "$MEMCG_SLICE" MemoryMax="${MEMCG_MAX_MIB}M" MemoryHigh="${MEMCG_HIGH_MIB}M" ||
    die "cannot set limits on $MEMCG_SLICE"
  [[ "$(systemctl show "$MEMCG_SLICE" -p MemoryMax --value)" == $((MEMCG_MAX_MIB << 20)) ]] || die "MemoryMax did not stick"
  log "memory cgroup $MEMCG_SLICE: MemoryMax=${MEMCG_MAX_MIB}M MemoryHigh=${MEMCG_HIGH_MIB}M (${MEMCG_PCT}% of $bytes B)"
}

memcg_snapshot() {   # $1 tag: live limits + members (proves placement) + usage counters
  { echo "unit_MemoryMax=$(systemctl show "$MEMCG_SLICE" -p MemoryMax --value)"
    echo "unit_MemoryHigh=$(systemctl show "$MEMCG_SLICE" -p MemoryHigh --value)"
    for f in memory.max memory.high memory.current memory.peak memory.swap.current; do
      echo "$f=$(cat "$MEMCG_DIR/$f" 2>/dev/null)"
    done
    sed 's/^/memory.events./; s/ /=/' "$MEMCG_DIR/memory.events" 2>/dev/null
    echo "# members: pid comm cgroup"
    memcg_procs
  } > "$CASE_DIR/memcg_$1.txt"
}

kill_own() {   # SIGTERM a PID this script started and its direct children (sudo/scope wrappers) - by PID only
  local p="$1" c
  [[ -n "$p" ]] || return 0
  for c in $(pgrep -P "$p" 2>/dev/null); do sudo kill "$c" 2>/dev/null; done
  sudo kill "$p" 2>/dev/null; true
}

memcg_stop() {   # stop whatever is left in OUR slice (units and scopes we created) - cgroup-scoped, never by name
  [[ -n "$(memcg_procs)" ]] && { log "stopping leftovers in $MEMCG_SLICE"; memcg_procs; }
  sudo systemctl stop "$MEMCG_SLICE" 2>/dev/null || true
}

# ---------------------------------------------------------------- capture / samplers
DISK_SAMPLER_PID=""; FS_SAMPLER_PID=""; MEMCG_SAMPLER_PID=""; BPF_PID=""

start_samplers() {
  { printf 'timestamp_s inflight_read inflight_write block_stat\n'
    while true; do
      printf '%s ' "$(date +%s.%N)"; tr '\n' ' ' < "/sys/block/$DEVNAME/inflight"; cat "/sys/block/$DEVNAME/stat"
      sleep "$DISK_SAMPLE_SECONDS"
    done; } 9>&- > "$CASE_DIR/data_device_timeseries.txt" &
  DISK_SAMPLER_PID=$!
  { printf 'timestamp_s session_write_kbytes delayed_allocation_blocks df_used_bytes df_avail_bytes\n'
    while true; do
      echo "$(date +%s.%N) $(cat "$SYSFS_DIR/session_write_kbytes") $(cat "$SYSFS_DIR/delayed_allocation_blocks") $(df -B1 --output=used,avail "$MNT" | tail -1)"
      sleep "$FS_SAMPLE_SECONDS"
    done; } 9>&- > "$CASE_DIR/fs_timeseries.txt" &
  FS_SAMPLER_PID=$!
  { printf 'timestamp_s memory_current memory_swap_current nprocs\n'
    while true; do
      echo "$(date +%s.%N) $(cat "$MEMCG_DIR/memory.current" 2>/dev/null || echo 0) $(cat "$MEMCG_DIR/memory.swap.current" 2>/dev/null || echo 0) $(memcg_procs | wc -l)"
      sleep "$FS_SAMPLE_SECONDS"
    done; } 9>&- > "$CASE_DIR/memcg_timeseries.txt" &
  MEMCG_SAMPLER_PID=$!
  if [[ "$BLKLAT" == 1 ]]; then
    local mm maj min
    mm="$(cat /sys/block/$DEVNAME/dev)"; maj=${mm%%:*}; min=${mm##*:}
    sudo BPFTRACE_MAP_KEYS_MAX=1048576 bpftrace "$APPS_DIR/blklat.bt" $(((maj << 20) | min)) \
      > "$CASE_DIR/blklat.txt" 2> "$CASE_DIR/blklat.err" &
    BPF_PID=$!
    for _ in $(seq 100); do grep -q '^blklat attached' "$CASE_DIR/blklat.txt" 2>/dev/null && break; sleep 0.1; done
    grep -q '^blklat attached' "$CASE_DIR/blklat.txt" || log "WARNING: bpftrace did not attach (see blklat.err)"
  fi
}

stop_samplers() {
  local pid
  for pid in "$DISK_SAMPLER_PID" "$FS_SAMPLER_PID" "$MEMCG_SAMPLER_PID"; do
    [[ -n "$pid" ]] && { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }
  done
  DISK_SAMPLER_PID=""; FS_SAMPLER_PID=""; MEMCG_SAMPLER_PID=""
  if [[ -n "$BPF_PID" ]]; then   # SIGINT to bpftrace itself (child of sudo) so it prints its maps
    local c; c="$(pgrep -P "$BPF_PID" 2>/dev/null | head -1)"
    if [[ -n "$c" ]]; then sudo kill -INT "$c"; else sudo kill -INT "$BPF_PID"; fi 2>/dev/null
    local i; for i in $(seq 120); do kill -0 "$BPF_PID" 2>/dev/null || break; sleep 0.5; done
    if kill -0 "$BPF_PID" 2>/dev/null; then   # never block the run (and the rmmod after it) on bpftrace
      log "WARNING: bpftrace (pid $BPF_PID) did not exit within 60 s; continuing without waiting"
      echo "bpftrace_hung=1" >> "$CASE_DIR/warnings.txt"
    else
      wait "$BPF_PID" 2>/dev/null
    fi
    BPF_PID=""
  fi
}

capture_state() {   # $1 phase
  local ph="$1"
  cat "/sys/block/$DEVNAME/stat" > "$CASE_DIR/${ph}_block_stat.txt"
  df -B1 "$MNT" > "$CASE_DIR/${ph}_df.txt"
  for f in lifetime_write_kbytes session_write_kbytes delayed_allocation_blocks reserved_clusters errors_count; do
    echo "$f=$(cat "$SYSFS_DIR/$f" 2>/dev/null)"
  done > "$CASE_DIR/${ph}_ext4_sysfs.txt"
  echo "df_util_pct=$(df_util)" >> "$CASE_DIR/${ph}_ext4_sysfs.txt"
  sudo cat /proc/fs/jbd2/"$DEVNAME"-8/info > "$CASE_DIR/${ph}_jbd2_info.txt" 2>/dev/null || true
  dm_snapshot "$ph"
}

capture_start() {
  capture_state start
  memcg_snapshot start
  start_samplers
  START_UTIL="$(df_util)"   # no sync here: it would add I/O inside the window (delalloc counts as used anyway)
  log "measurement start utilisation=${START_UTIL:-?}% (window ${UTIL_MIN}-${UTIL_MAX})"
  if [[ -z "$START_UTIL" ]] || ((START_UTIL < UTIL_MIN || START_UTIL > UTIL_MAX)); then
    echo "util_out_of_window=${START_UTIL:-unknown}" >> "$CASE_DIR/warnings.txt"
    [[ "$UTIL_CHECK" == abort ]] && { log "[FATAL] start utilisation ${START_UTIL:-?}% outside ${UTIL_MIN}-${UTIL_MAX}%"; return 1; }
  fi
  return 0
}

capture_end() {
  stop_samplers
  memcg_snapshot end
  capture_state end
}
