#!/bin/bash
# tools/apps/run_all.sh - 8 workloads x 6 policies on NVMeVirt, resumable.
#   tools/apps/run_all.sh                       # everything not yet DONE, workload-major order (as the study)
#   WORKLOADS="tpcc ycsba" POLICIES="page4k proposed" tools/apps/run_all.sh
#   RETRY_FAILED=1 tools/apps/run_all.sh        # also re-run cases whose STATUS is FAILED:*
#   tools/apps/run_all.sh --status              # print the matrix state and exit
#   tools/apps/run_all.sh --cleanup             # after a killed run: stop workloads, umount /mnt/dmapp, rmmod (ours only)
# State lives in the case dirs (results/apps/<wl>/<pol>/STATUS) plus an append-only log results/apps/state.log;
# a case left RUNNING by a crash/reboot is re-run. B18 is computed once (page4k load) and cached in
# results/apps/b18_<memmap_size>.txt, then passed to every run.
# Stops (does not continue) on kernel trouble in dmesg, like the study's queue guard.
set -uo pipefail
APPS="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
MNT=/mnt/dmapp
. "$APPS/common.sh"
WORKLOADS=(${WORKLOADS:-vmhot1g oltp fileserverfs tpcc ycsba ycsbf fio_randwrite fio_mix})
POLICIES=(${POLICIES:-${ALL_POLICIES[*]}})
STATE_LOG="$RESULTS_ROOT/state.log"
mkdir -p "$RESULTS_ROOT"

status_of() { cat "$RESULTS_ROOT/$1/$2/STATUS" 2>/dev/null || echo PENDING; }

if [[ "${1:-}" == --status ]]; then
  printf '%-14s' workload; printf '%-12s' "${POLICIES[@]}"; echo
  for wl in "${WORKLOADS[@]}"; do
    printf '%-14s' "$wl"; for pol in "${POLICIES[@]}"; do printf '%-12s' "$(status_of "$wl" "$pol" | cut -c1-11)"; done; echo
  done
  exit 0
fi

if [[ "${1:-}" == --cleanup ]]; then   # after a killed run: unmount /mnt/dmapp and unload nvmev, only if it is ours
  src="$(findmnt -rn -o SOURCE "$MNT" 2>/dev/null || true)"
  if [[ -n "$src" ]]; then
    m="$(lsblk -dn -o MODEL "$src" 2>/dev/null | sed 's/ *$//')"
    [[ "$m" == ${NVMEV_MODEL_PREFIX}* ]] || die "$MNT is mounted from $src ($m) - not ours"
    # never kill by process name: stop our own slice (all our units/scopes) and our own PostgreSQL clusters only
    sudo systemctl stop dmapp.slice
    for u in $(systemctl list-units --plain --no-legend 'dmapp*' | awk '{print $1}'); do sudo systemctl stop "$u"; done
    for d in "$MNT"/postgresql/16/inst_* "$MNT"/postgresql_tpccmulti/*/main; do
      [[ -d "$d" ]] && sudo -u postgres /usr/lib/postgresql/16/bin/pg_ctl -D "$d" stop -m immediate >/dev/null 2>&1
    done
    sleep 2
    sudo umount "$MNT" || { sudo fuser -vm "$MNT"; die "umount failed: stop the processes above (by PID) and retry"; }
    [[ -z "$(other_nvmev_users)" ]] && nvmev_loaded && sudo rmmod nvmev
    log "cleaned up"; exit 0
  fi
  nvmev_loaded && die "nvmev is loaded but $MNT is not mounted - cannot tell whose it is; rmmod by hand if it is ours"
  log "nothing to clean"; exit 0
fi

[[ -z "$(other_nvmev_users)" ]] || die "an nvmev_cycle/randwrite/mixed run is active; wait for it"
nvmev_loaded && die "nvmev is loaded by someone else; not starting"
findmnt -rn "$MNT" >/dev/null 2>&1 && die "$MNT is mounted; clean up first"

GOV_BEFORE="$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null)"
restore() {   # unload a module this script itself loaded (B18 probe); put the CPU governor back
  nvmev_rmmod
  if [[ -n "$GOV_BEFORE" && -n "$CPU_GOVERNOR" ]]; then
    for p in /sys/devices/system/cpu/cpufreq/policy*; do echo "$GOV_BEFORE" | sudo tee "$p/scaling_governor" >/dev/null; done
  fi
}
trap restore EXIT
B18_FILE="$RESULTS_ROOT/b18_${NVMEV_MEMMAP_SIZE}.txt"
if [[ -s "$B18_FILE" ]]; then
  B18="$(cat "$B18_FILE")"
else
  CASE_DIR="$RESULTS_ROOT"; compute_b18; echo "$B18" > "$B18_FILE"
fi
export B18
log "B18=$B18 (cached in $B18_FILE)"

BAD='kernel BUG|Oops:|soft lockup|hung_task|blocked for more than|WARNING:.*nvmev'
for wl in "${WORKLOADS[@]}"; do
  for pol in "${POLICIES[@]}"; do
    s="$(status_of "$wl" "$pol")"
    [[ "$s" == DONE ]] && continue
    [[ "$s" == FAILED* && "${RETRY_FAILED:-0}" != 1 ]] && { log "skip $wl/$pol ($s)"; continue; }
    [[ -z "$(other_nvmev_users)" ]] || { log "another NVMeVirt experiment started - stopping"; exit 1; }
    nvmev_loaded && { log "nvmev loaded by someone else - stopping"; exit 1; }
    echo "$(date -u +%FT%TZ) START $wl $pol" >> "$STATE_LOG"
    "$APPS/run_app.sh" "$wl" "$pol"
    echo "$(date -u +%FT%TZ) END $wl $pol $(status_of "$wl" "$pol")" >> "$STATE_LOG"
    if grep -qE "$BAD" "$RESULTS_ROOT/$wl/$pol/dmesg.txt" 2>/dev/null; then
      log "kernel trouble during $wl/$pol (see dmesg.txt) - stopping the matrix"; exit 1
    fi
    nvmev_loaded && { log "nvmev still loaded after $wl/$pol - stopping"; exit 1; }
  done
done
python3 "$APPS/summarize.py" table "$RESULTS_ROOT" > "$RESULTS_ROOT/summary.tsv"
log "matrix finished; table in $RESULTS_ROOT/summary.tsv"
