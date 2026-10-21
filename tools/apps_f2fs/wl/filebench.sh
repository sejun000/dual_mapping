# wl/filebench.sh - splitEval workloads/filebench.sh on the dmapp chassis.
# Uses the study's conv .f files verbatim (copies in tools/apps/filebench/, sha256 identical to the study's
# runs) with three textual substitutions, all recorded in the case dir:
#   * set $dir=/mnt/f2fs/test        -> $MNT/test
#   * set $nfiles=<n>                -> round(n * FB_SCALE)  (the cold fileset only; the 1 GB hot set and every
#                                       other parameter are untouched). FB_SCALE=auto scales by F2FS user
#                                       capacity (ours / the study's 18440 user segments) so "fill 50%" stays 50%.
#   * psrun 600 600                  -> psrun $DURATION $DURATION   (only if DURATION != 600)
# filebench creates its filesets inside the timed process; capture starts at its "Running..." marker.

WL_KIND="filebench-${FB_PROFILE:?}"
WL_SELF_CAPTURE=1
WL_FILL=0
UTIL_MIN="${UTIL_MIN:-48}"; UTIL_MAX="${UTIL_MAX:-80}"   # study: 48 (fill 50 - 2) .. EXPECTED_F2FS_USE_MAX=80 (CONV)
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
  local scale n0 n1
  if [[ "$FB_SCALE" == auto ]]; then
    scale="$(python3 -c "print(${F2FS_USER_SEGS:?} / $STUDY_USER_SEGS)")"
  else
    scale="$FB_SCALE"
  fi
  n0="$(sed -nE 's/^set \$nfiles=([0-9]+).*/\1/p' "$FB_SRC")"
  n1="$(python3 -c "print(max(1, round($n0 * $scale)))")"
  FB_FILE="$CASE_DIR/$(basename "$FB_SRC")"
  sed -e "s|^set \$dir=/mnt/f2fs/test|set \$dir=$MNT/test|" \
      -e "s|^set \$nfiles=$n0|set \$nfiles=$n1|" \
      -e "s|^psrun 600 600|psrun $DURATION $DURATION|" "$FB_SRC" > "$FB_FILE"
  grep -q "^set \$dir=$MNT/test" "$FB_FILE" || die "dir substitution failed"
  printf 'fb_scale=%s\nfb_nfiles_study=%s\nfb_nfiles=%s\nf2fs_user_segs=%s\n' "$scale" "$n0" "$n1" "$F2FS_USER_SEGS" \
    >> "$CASE_DIR/workload_config.txt"
  diff "$FB_SRC" "$FB_FILE" > "$CASE_DIR/fb_substitutions.diff"
  log "[filebench] $FB_PROFILE: nfiles $n0 -> $n1 (scale $scale), psrun $DURATION"
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
