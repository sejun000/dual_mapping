# wl/filebench.sh - splitEval workloads/filebench.sh on the dmapp chassis.
# Uses the study's conv .f files verbatim (copies in tools/apps/filebench/, sha256 identical to the study's
# runs) with three textual substitutions, all recorded in the case dir:
#   * set $dir=/mnt/f2fs/test        -> $MNT/test
#   * set $nfiles=<n>                -> the count that fills ext4 to FB_FILL_PCT (50) df Use% (fb_nfiles.py: each
#                                       preallocated file = its size rounded to 4 KiB; the cold fileset only - the
#                                       1 GB hot set and every other parameter are untouched). FB_SCALE=<x> instead
#                                       uses round(study n * x) (smoke/override).
#   * psrun 600 600                  -> psrun $DURATION $DURATION   (only if DURATION != 600)
# filebench creates its filesets inside the timed process; capture starts at its "Running..." marker.

WL_KIND="filebench-${FB_PROFILE:?}"
WL_SELF_CAPTURE=1
WL_FILL=0                                               # the fill is the filesets themselves (no ballast)
FB_FILL_PCT="${FB_FILL_PCT:-50}"
UTIL_MIN="${UTIL_MIN:-$((FB_FILL_PCT - 3))}"; UTIL_MAX="${UTIL_MAX:-$((FB_FILL_PCT + 5))}"
DURATION="${DURATION:-600}"
FILEBENCH="${FILEBENCH:-/home/sejun000/splitFTL/gyjin/distfiles/filebench/filebench}"
FILEBENCH_BASE_SEED="${FILEBENCH_BASE_SEED:-4357}"
FB_SCALE="${FB_SCALE:-auto}"
FB_SRC="$APPS_DIR/filebench/${FB_PROFILE}-50-perc-conv-fsutil-600s.f"
STARTUP_TIMEOUT_SECONDS="${STARTUP_TIMEOUT_SECONDS:-3600}"
FB_UNIT=""

wl_check_deps() {
  [[ -x "$FILEBENCH" ]] || die "missing filebench: $FILEBENCH"
  [[ -f "$FB_SRC" ]] || die "missing $FB_SRC"
}

wl_setup() {
  local scale n0 n1 used fixed perfile
  n0="$(sed -nE 's/^set \$nfiles=([0-9]+).*/\1/p' "$FB_SRC")"
  if [[ "$FB_SCALE" == auto ]]; then
    sudo sync; used="$(df -B1 --output=used "$MNT" | tail -1 | tr -d ' ')"
    read -r n1 fixed perfile < <(python3 "$APPS_DIR/fb_nfiles.py" "$FB_SRC" "$FB_FILL_PCT" "$FS_CAPACITY" "$used")
    scale="auto(${FB_FILL_PCT}% of $FS_CAPACITY B; fixed $fixed B, $perfile B/entry)"
  else
    scale="$FB_SCALE"
    n1="$(python3 -c "print(max(1, round($n0 * $scale)))")"
  fi
  FB_FILE="$CASE_DIR/$(basename "$FB_SRC")"
  sed -e "s|^set \$dir=/mnt/f2fs/test|set \$dir=$MNT/test|" \
      -e "s|^set \$nfiles=$n0|set \$nfiles=$n1|" \
      -e "s|^psrun 600 600|psrun $DURATION $DURATION|" "$FB_SRC" > "$FB_FILE"
  grep -q "^set \$dir=$MNT/test" "$FB_FILE" || die "dir substitution failed"
  printf 'fb_scale=%s\nfb_nfiles_study=%s\nfb_nfiles=%s\nfb_fill_pct=%s\n' "$scale" "$n0" "$n1" "$FB_FILL_PCT" \
    >> "$CASE_DIR/workload_config.txt"
  diff "$FB_SRC" "$FB_FILE" > "$CASE_DIR/fb_substitutions.diff"
  log "[filebench] $FB_PROFILE: nfiles $n0 -> $n1 ($scale), psrun $DURATION"
}

wl_run() {
  local logfile="$CASE_DIR/filebench_run.log" pid
  FB_UNIT="dmapp-filebench-$$"
  sudo systemctl reset-failed "$FB_UNIT.service" >/dev/null 2>&1 || true
  { sudo systemd-run --unit="$FB_UNIT" \
      --slice="$MEMCG_SLICE" \
      --property="WorkingDirectory=$CASE_DIR" \
      --setenv="FILEBENCH_BASE_SEED=$FILEBENCH_BASE_SEED" --pipe \
      /usr/bin/setarch "$(uname -m)" --addr-no-randomize \
      $NUMACTL "$FILEBENCH" -f "$FB_FILE"; } > "$logfile" 2>&1 &
  pid=$!
  for _ in $(seq $((STARTUP_TIMEOUT_SECONDS * 10))); do
    grep -Eq '^[0-9]+\.[0-9]+: Running\.\.\.$' "$logfile" && break
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  if ! grep -Eq '^[0-9]+\.[0-9]+: Running\.\.\.$' "$logfile"; then
    log "[FATAL] filebench never reached Running... (see filebench_run.log)"
    WL_STATUS=1; return 0
  fi
  capture_start || { WL_STATUS=2; return 0; }
  WL_STATUS=0
  wait "$pid" || WL_STATUS=$?
  FB_UNIT=""
}

wl_ok() { grep -q 'IO Summary:' "$CASE_DIR/filebench_run.log"; }

wl_teardown() {
  if [[ -n "$FB_UNIT" ]]; then
    sudo systemctl kill --kill-whom=all --signal=SIGINT "$FB_UNIT.service" >/dev/null 2>&1 || true
    sudo systemctl stop "$FB_UNIT.service" >/dev/null 2>&1 || true
    FB_UNIT=""
  fi
}

wl_config_lines() {
  printf 'filebench=%s\nfilebench_profile=%s\nfilebench_base_seed=%s\nfilebench_src=%s\n' \
    "$FILEBENCH" "$FB_PROFILE" "$FILEBENCH_BASE_SEED" "$FB_SRC"
  sha256sum "$FILEBENCH" "$FB_SRC"
}
