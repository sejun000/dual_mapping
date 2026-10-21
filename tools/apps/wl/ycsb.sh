# wl/ycsb.sh - splitEval workloads/ycsb_multi.sh as configured by tests/mn_campaign_queue.sh (ycsb320a/ycsb320f):
#   WORKLOAD=ycsb-multi DURATION=600 FILL_TARGET_PERCENT=75(ext4: df 76) SYNC_COMMIT=on YCSBM_INSTANCES=16 THREADS=320
#   RECORDCOUNT=10000000 MAX_CONNECTIONS=64 YCSBM_WORKLOAD=workloada|workloadf
# plus the plugin defaults: LOAD_PROCS=8 (total), POSTGRES_FSYNC=on, wal_sync_method=open_datasync,
# POSTGRES_PROFILE=metadata-pressure, YCSB_CORE_OPTIONS="readproportion=0.5 updateproportion=0.5" (also applied
# to workloadf, as in the study), threaded client, JVM -Xms64m -Xmx256m ... ActiveProcessorCount=16.
# 16 PostgreSQL 16 clusters on one mount (pg_ctl), ports 5433+. Every postgres_run.sh call (pg_ctl -> postgres,
# YCSB loader/client JVMs) runs through $SCOPE, i.e. inside the common dmapp.slice (the study had no cgroup here).
# MAX_CONNECTIONS is accepted for parity but, as in the study, postgres_run.sh hard-codes max_connections=300.

WL_KIND=ycsb-multi
WL_SELF_CAPTURE=0
WL_FILL="${FILL_TARGET_PERCENT:-76}"   # ext4: df Use% target = the study's PostgreSQL start utilisation (76%)
UTIL_MIN="${UTIL_MIN:-$((WL_FILL - 3))}"; UTIL_MAX="${UTIL_MAX:-85}"   # the load alone may land above 76%
DURATION="${DURATION:-600}"
YCSBM_INSTANCES="${YCSBM_INSTANCES:-16}"
YCSBM_WORKLOAD="${YCSBM_WORKLOAD:?}"
RECORDCOUNT="${RECORDCOUNT:-10000000}"
THREADS="${THREADS:-320}"
LOAD_PROCS="${LOAD_PROCS:-8}"
SYNC_COMMIT="${SYNC_COMMIT:-on}"
POSTGRES_FSYNC="${POSTGRES_FSYNC:-on}"
POSTGRES_WAL_SYNC_METHOD="${POSTGRES_WAL_SYNC_METHOD:-open_datasync}"
POSTGRES_PROFILE="${POSTGRES_PROFILE:-metadata-pressure}"
MAX_CONNECTIONS="${MAX_CONNECTIONS:-64}"
YCSB_CORE_OPTIONS="${YCSB_CORE_OPTIONS:-readproportion=0.5 updateproportion=0.5}"
YCSB_JAVA_OPTS="${YCSB_JAVA_OPTS:--Xms64m -Xmx256m -XX:MaxMetaspaceSize=128m -XX:+UseSerialGC -XX:ActiveProcessorCount=16}"
# reporting only: more percentiles from the same HdrHistogram (study default printed 95,99)
YCSB_EXTRA_OPTIONS="${YCSB_EXTRA_OPTIONS:-hdrhistogram.percentiles=50,90,95,99,99.9}"
YCSB_BASE="${YCSB_BASE:-/home/sejun000/splitFTL/gyjin/distfiles/YCSB/distribution/target/ycsb-0.16.0}"
YCSBM_PORT_BASE="${YCSBM_PORT_BASE:-5433}"
PG_BIN=/usr/lib/postgresql/16/bin
PG_ROOT="$MNT/postgresql"
YCSBM_BASE="$PG_ROOT/16"
PGLOG_DIR=/tmp/dmapp_pg
PR="$APPS_DIR/pg_ycsb.sh"
YCSB_PIDS=()

ycsbm_recs()    { echo $((RECORDCOUNT / YCSBM_INSTANCES)); }
ycsbm_threads() { local t=$((THREADS / YCSBM_INSTANCES)); ((t >= 1)) || t=1; echo "$t"; }
ycsbm_loaders() { local l=$((LOAD_PROCS / YCSBM_INSTANCES)); ((l >= 1)) || l=1; echo "$l"; }

ycsbm_call() {   # instance mode [workload]
  local i="$1" mode="$2" wl="${3:-}"
  $SCOPE env "HOME=$HOME" PG_MULTI=1 "PG_ROOT=$PG_ROOT" "PGDATA=$YCSBM_BASE/inst_$i" "PG_PORT=$((YCSBM_PORT_BASE + i - 1))" \
    "PG_DB=ycsb" "PG_LOG=$PGLOG_DIR/pg_ycsbm_$i.log" "YCSB_BASE=$YCSB_BASE" \
    "RECORDCOUNT=$(ycsbm_recs)" "OPERATIONCOUNT=1000000000" \
    "THREADS=$(ycsbm_threads)" "LOAD_PROCS=$(ycsbm_loaders)" "YCSB_CLIENT_MODEL=threaded" \
    "SYNC_COMMIT=$SYNC_COMMIT" "POSTGRES_FSYNC=$POSTGRES_FSYNC" \
    "POSTGRES_WAL_SYNC_METHOD=$POSTGRES_WAL_SYNC_METHOD" "POSTGRES_PROFILE=$POSTGRES_PROFILE" \
    "POSTGRES_EXTRA_CONF=${POSTGRES_EXTRA_CONF:-}" \
    "YCSB_CORE_OPTIONS=$YCSB_CORE_OPTIONS" "YCSB_EXTRA_OPTIONS=$YCSB_EXTRA_OPTIONS" \
    "JAVA_OPTS=$YCSB_JAVA_OPTS" ${RUN_MAXSECONDS:+"RUN_MAXSECONDS=$RUN_MAXSECONDS"} \
    $NUMACTL "$PR" "$mode" $wl
}

wl_check_deps() {
  [[ -x "$PG_BIN/initdb" ]] || die "missing PostgreSQL 16 ($PG_BIN)"
  [[ -x "$YCSB_BASE/bin/ycsb.sh" ]] || die "missing YCSB at $YCSB_BASE"
  [[ -f "$YCSB_BASE/workloads/$YCSBM_WORKLOAD" ]] || die "missing YCSB workload $YCSBM_WORKLOAD"
  local i p; for i in $(seq "$YCSBM_INSTANCES"); do p=$((YCSBM_PORT_BASE + i - 1))
    port_in_use "$p" && die "port $p already in use"; done; return 0
}

wl_setup() {
  local i pids=() failed=0
  mkdir -p "$PGLOG_DIR"; sudo chmod 1777 "$PGLOG_DIR"; sudo rm -f "$PGLOG_DIR"/pg_ycsbm_*.log
  sudo rm -rf "$YCSBM_BASE"; sudo mkdir -p "$PG_ROOT"
  log "[ycsb] $YCSBM_INSTANCES instances x (records=$(ycsbm_recs) threads=$(ycsbm_threads) $YCSBM_WORKLOAD)"
  for i in $(seq "$YCSBM_INSTANCES"); do ycsbm_call "$i" start > "$CASE_DIR/pg_start_$i.log" 2>&1 & pids+=($!); done
  for i in "${!pids[@]}"; do wait "${pids[$i]}" || failed=1; done
  ((failed)) && die "instance start failed (see pg_start_*.log)"
  pids=()
  for i in $(seq "$YCSBM_INSTANCES"); do ycsbm_call "$i" load "$YCSBM_WORKLOAD" > "$CASE_DIR/ycsb_load_$i.log" 2>&1 & pids+=($!); done
  for i in "${!pids[@]}"; do wait "${pids[$i]}" || failed=1; done
  ((failed)) && die "load failed (see ycsb_load_*.log)"
  return 0
}

wl_post_ballast() {
  local i
  for i in $(seq "$YCSBM_INSTANCES"); do
    sudo -u postgres "$PG_BIN/psql" -h 127.0.0.1 -p $((YCSBM_PORT_BASE + i - 1)) -d ycsb -c 'CHECKPOINT;' >/dev/null
  done
}

wl_run() {
  local i pids=()
  log "[ycsb] run ${DURATION}s, $YCSBM_INSTANCES x $(ycsbm_threads) threads"
  WL_STATUS=0
  for i in $(seq "$YCSBM_INSTANCES"); do
    RUN_MAXSECONDS="$DURATION" ycsbm_call "$i" run "$YCSBM_WORKLOAD" > "$CASE_DIR/ycsb_run_$i.log" 2>&1 & pids+=($!)
  done
  YCSB_PIDS=("${pids[@]}")
  for i in "${!pids[@]}"; do wait "${pids[$i]}" || WL_STATUS=$?; done
  YCSB_PIDS=()
}

wl_ok() {
  local i
  for i in $(seq "$YCSBM_INSTANCES"); do grep -q '\[OVERALL\], Throughput' "$CASE_DIR/ycsb_run_$i.log" || return 1; done
}

wl_teardown() {
  local i
  for p in "${YCSB_PIDS[@]}"; do kill_own "$p"; done; YCSB_PIDS=()   # our own client subshells and their sudo/scope children
  for i in $(seq "$YCSBM_INSTANCES"); do
    ycsbm_call "$i" end >/dev/null 2>&1 || true
    sudo -u postgres "$PG_BIN/pg_ctl" -D "$YCSBM_BASE/inst_$i" stop -m immediate >/dev/null 2>&1 || true
  done
  sudo cp "$PGLOG_DIR"/pg_ycsbm_*.log "$CASE_DIR/" 2>/dev/null && sudo chown "$USER" "$CASE_DIR"/pg_ycsbm_*.log; true
}

wl_config_lines() {
  printf 'instances=%s\nrecordcount_total=%s\nthreads_total=%s\nload_procs_total=%s\nycsb_workload=%s\n' \
    "$YCSBM_INSTANCES" "$RECORDCOUNT" "$THREADS" "$LOAD_PROCS" "$YCSBM_WORKLOAD"
  printf 'ycsb_core_options=%s\nycsb_extra_options=%s\nycsb_java_opts=%s\n' "$YCSB_CORE_OPTIONS" "$YCSB_EXTRA_OPTIONS" "$YCSB_JAVA_OPTS"
  printf 'synchronous_commit=%s\nfsync=%s\nwal_sync_method=%s\npostgres_profile=%s\npostgres_extra_conf=%s\n' \
    "$SYNC_COMMIT" "$POSTGRES_FSYNC" "$POSTGRES_WAL_SYNC_METHOD" "$POSTGRES_PROFILE" "${POSTGRES_EXTRA_CONF:-none}"
  printf 'postgres_version=%s\njava_version=%s\n' "$("$PG_BIN/postgres" --version)" "$(java -version 2>&1 | head -1)"
}
