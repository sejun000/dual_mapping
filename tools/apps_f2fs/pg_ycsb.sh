#!/usr/bin/env bash
set -euo pipefail
#
# pg_ycsb.sh — copy of splitEval scripts/ycsb_run_scripts/postgres_run.sh (YCSB on PostgreSQL, self-managed
# cluster on the nvmev-backed f2fs). Changes for tools/apps: the data root is PG_ROOT (was /mnt/f2fs/postgresql),
# and YCSB_EXTRA_OPTIONS (reporting-only properties, e.g. hdrhistogram.percentiles) are appended to load/run.
# Every PostgreSQL / YCSB parameter is unchanged.
# POSTGRES_PROFILE selects either the historical setup or a reproducible metadata-pressure
# setup that forces more relation writeback and checkpoints without changing PostgreSQL code.
# Runs its own cluster via pg_ctl (NOT systemctl) so all data lands on /mnt/f2fs (device 1).
#
if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <start|load|run|cleanup|end> [workload]" >&2
  exit 1
fi
MODE="$1"; shift || true

RECORDCOUNT=${RECORDCOUNT:-2000000}
OPERATIONCOUNT=${OPERATIONCOUNT:-10000000}
INSERTCOUNT=${INSERTCOUNT:-$RECORDCOUNT}
THREADS=${THREADS:-16}
YCSB_CLIENT_MODEL=${YCSB_CLIENT_MODEL:-threaded}

# --- YCSB paths (correct for THIS tree; old scripts pointed at /home/gyjin) ---
YCSB_ROOT_BASE=${YCSB_ROOT_BASE:-"/home/sejun000/splitFTL/gyjin/distfiles/YCSB"}
YCSB_BASE=${YCSB_BASE:-"/home/sejun000/splitFTL/gyjin/distfiles/YCSB/distribution/target/ycsb-0.16.0"}
YCSB_BIN="$YCSB_BASE/bin/ycsb.sh"

# --- PostgreSQL (self-managed) ---
PG_BIN=${PG_BIN:-/usr/lib/postgresql/16/bin}
PG_ROOT=${PG_ROOT:?PG_ROOT must be set (e.g. /mnt/dmapp/postgresql)}
PGDATA=${PGDATA:-$PG_ROOT/16/main}
PG_HOST=${PG_HOST:-127.0.0.1}
PG_PORT=${PG_PORT:-5432}
PG_DB=${PG_DB:-ycsb}
PG_LOG=${PG_LOG:-/tmp/pg_ycsb_server.log}
SYNC_COMMIT=${SYNC_COMMIT:-off}      # 'on' = per-commit WAL fsync (fragments WAL); 'off' = async (batched)
POSTGRES_FSYNC=${POSTGRES_FSYNC:-on}
POSTGRES_WAL_SYNC_METHOD=${POSTGRES_WAL_SYNC_METHOD:-fdatasync}
POSTGRES_PROFILE=${POSTGRES_PROFILE:-legacy}

case "$POSTGRES_PROFILE" in
  legacy)
    profile_shared_buffers=512MB
    profile_full_page_writes=off
    profile_wal_writer_delay=200ms
    profile_bgwriter_delay=10ms
    profile_bgwriter_lru_maxpages=1000
    profile_bgwriter_lru_multiplier=10.0
    profile_checkpoint_timeout=2min
    profile_checkpoint_completion_target=0.9
    profile_max_wal_size=2GB
    profile_min_wal_size=160MB
    profile_autovacuum_max_workers=4
    profile_autovacuum_naptime=10s
    profile_autovacuum_vacuum_scale_factor=0.02
    profile_autovacuum_analyze_scale_factor=0.1
    profile_table_fillfactor=80
    ;;
  metadata-pressure)
    # Keep PostgreSQL durability enabled while increasing 8 KiB relation writeback and
    # checkpoint/NAT/SIT churn. This is intentionally a named experiment profile.
    profile_shared_buffers=128MB
    profile_full_page_writes=on
    profile_wal_writer_delay=10ms
    profile_bgwriter_delay=10ms
    profile_bgwriter_lru_maxpages=10000
    profile_bgwriter_lru_multiplier=10.0
    profile_checkpoint_timeout=30s
    profile_checkpoint_completion_target=0.9
    profile_max_wal_size=2GB
    profile_min_wal_size=80MB
    profile_autovacuum_max_workers=8
    profile_autovacuum_naptime=1s
    profile_autovacuum_vacuum_scale_factor=0.005
    profile_autovacuum_analyze_scale_factor=0.005
    profile_table_fillfactor=70
    ;;
  *)
    echo "Unknown POSTGRES_PROFILE: $POSTGRES_PROFILE" >&2
    exit 2
    ;;
esac

SHARED_BUFFERS=${SHARED_BUFFERS:-$profile_shared_buffers}
FULL_PAGE_WRITES=${FULL_PAGE_WRITES:-$profile_full_page_writes}
WAL_WRITER_DELAY=${WAL_WRITER_DELAY:-$profile_wal_writer_delay}
BGWRITER_DELAY=${BGWRITER_DELAY:-$profile_bgwriter_delay}
BGWRITER_LRU_MAXPAGES=${BGWRITER_LRU_MAXPAGES:-$profile_bgwriter_lru_maxpages}
BGWRITER_LRU_MULTIPLIER=${BGWRITER_LRU_MULTIPLIER:-$profile_bgwriter_lru_multiplier}
CHECKPOINT_TIMEOUT=${CHECKPOINT_TIMEOUT:-$profile_checkpoint_timeout}
CHECKPOINT_COMPLETION_TARGET=${CHECKPOINT_COMPLETION_TARGET:-$profile_checkpoint_completion_target}
MAX_WAL_SIZE=${MAX_WAL_SIZE:-$profile_max_wal_size}
MIN_WAL_SIZE=${MIN_WAL_SIZE:-$profile_min_wal_size}
AUTOVACUUM_MAX_WORKERS=${AUTOVACUUM_MAX_WORKERS:-$profile_autovacuum_max_workers}
AUTOVACUUM_NAPTIME=${AUTOVACUUM_NAPTIME:-$profile_autovacuum_naptime}
AUTOVACUUM_VACUUM_SCALE_FACTOR=${AUTOVACUUM_VACUUM_SCALE_FACTOR:-$profile_autovacuum_vacuum_scale_factor}
AUTOVACUUM_ANALYZE_SCALE_FACTOR=${AUTOVACUUM_ANALYZE_SCALE_FACTOR:-$profile_autovacuum_analyze_scale_factor}
TABLE_FILLFACTOR=${TABLE_FILLFACTOR:-$profile_table_fillfactor}

if [[ "$POSTGRES_FSYNC" != on && "$POSTGRES_FSYNC" != off ]]; then
  echo "POSTGRES_FSYNC must be 'on' or 'off'" >&2
  exit 2
fi
if ! [[ "$TABLE_FILLFACTOR" =~ ^[0-9]+$ ]] ||
   (( TABLE_FILLFACTOR < 10 || TABLE_FILLFACTOR > 100 )); then
  echo "TABLE_FILLFACTOR must be an integer from 10 through 100" >&2
  exit 2
fi

pgctl() { sudo -u postgres "$PG_BIN/pg_ctl" -D "$PGDATA" "$@"; }
psql_super() { sudo -u postgres "$PG_BIN/psql" -h "$PG_HOST" -p "$PG_PORT" -v ON_ERROR_STOP=1 "$@"; }

stop_any() {
  pgctl stop -m immediate >/dev/null 2>&1 || true
  # PG_MULTI=1: several postmasters share the box - only ever touch our own
  # PGDATA; the global pkill would murder the sibling instances.
  if [ -z "${PG_MULTI:-}" ]; then
    :   # tools/apps: no name-based kill (was: pkill -9 -x postgres)
  fi
  sleep 1
}

case "$MODE" in
  start)
    stop_any
    sudo rm -rf "$PGDATA"
    sudo mkdir -p "$PGDATA"
    sudo chown -R postgres:postgres "$PG_ROOT"
    sudo chmod 700 "$PGDATA"

    sudo -u postgres "$PG_BIN/initdb" -D "$PGDATA" -E UTF8 --no-locale >/dev/null

    # ===== experiment profile (appended, so these values override defaults) =====
    sudo -u postgres tee -a "$PGDATA/postgresql.conf" >/dev/null <<EOF

# ===== YCSB PostgreSQL profile: $POSTGRES_PROFILE =====
listen_addresses = '127.0.0.1'
port = $PG_PORT
shared_buffers = $SHARED_BUFFERS
synchronous_commit = $SYNC_COMMIT
full_page_writes = $FULL_PAGE_WRITES
wal_writer_delay = $WAL_WRITER_DELAY
bgwriter_delay = $BGWRITER_DELAY
bgwriter_lru_maxpages = $BGWRITER_LRU_MAXPAGES
bgwriter_lru_multiplier = $BGWRITER_LRU_MULTIPLIER
checkpoint_timeout = $CHECKPOINT_TIMEOUT
checkpoint_completion_target = $CHECKPOINT_COMPLETION_TARGET
max_wal_size = $MAX_WAL_SIZE
min_wal_size = $MIN_WAL_SIZE
fsync = $POSTGRES_FSYNC
wal_sync_method = '$POSTGRES_WAL_SYNC_METHOD'
max_connections = 300
unix_socket_directories = '/tmp'
autovacuum = on
autovacuum_max_workers = $AUTOVACUUM_MAX_WORKERS
autovacuum_naptime = $AUTOVACUUM_NAPTIME
autovacuum_vacuum_scale_factor = $AUTOVACUUM_VACUUM_SCALE_FACTOR
autovacuum_analyze_scale_factor = $AUTOVACUUM_ANALYZE_SCALE_FACTOR
autovacuum_vacuum_cost_delay = 0
EOF

    # optional raw postgresql.conf lines (newline-separated), e.g. the
    # writeback-flush knobs: POSTGRES_EXTRA_CONF=$'backend_flush_after = 256kB'
    if [ -n "${POSTGRES_EXTRA_CONF:-}" ]; then
      printf '\n# ===== POSTGRES_EXTRA_CONF =====\n%s\n' "$POSTGRES_EXTRA_CONF" |
        sudo -u postgres tee -a "$PGDATA/postgresql.conf" >/dev/null
    fi

    # trust auth: the bundled JDBC 9.4 driver cannot do scram-sha-256 (PG16 default)
    sudo -u postgres tee "$PGDATA/pg_hba.conf" >/dev/null <<EOF
local   all   all                 trust
host    all   all   127.0.0.1/32  trust
host    all   all   ::1/128       trust
EOF

    pgctl -l "$PG_LOG" -w -t 120 start

    actual_fsync="$(psql_super -At -d postgres -c 'SHOW fsync')"
    actual_sync_commit="$(psql_super -At -d postgres -c 'SHOW synchronous_commit')"
    actual_wal_sync_method="$(psql_super -At -d postgres -c 'SHOW wal_sync_method')"
    actual_full_page_writes="$(psql_super -At -d postgres -c 'SHOW full_page_writes')"
    if [[ "$actual_fsync" != "$POSTGRES_FSYNC" ||
          "$actual_sync_commit" != "$SYNC_COMMIT" ||
          "$actual_wal_sync_method" != "$POSTGRES_WAL_SYNC_METHOD" ||
          "$actual_full_page_writes" != "$FULL_PAGE_WRITES" ]]; then
      echo "[postgres_run] configuration mismatch: fsync=$actual_fsync synchronous_commit=$actual_sync_commit wal_sync_method=$actual_wal_sync_method full_page_writes=$actual_full_page_writes" >&2
      exit 1
    fi

    psql_super -d postgres <<EOF
DROP DATABASE IF EXISTS $PG_DB;
CREATE DATABASE $PG_DB;
EOF
    psql_super -d "$PG_DB" <<EOF
CREATE TABLE usertable (YCSB_KEY VARCHAR(255) PRIMARY KEY NOT NULL, YCSB_VALUE JSONB NOT NULL)
  WITH (fillfactor=$TABLE_FILLFACTOR,
        autovacuum_vacuum_scale_factor=$AUTOVACUUM_VACUUM_SCALE_FACTOR,
        autovacuum_analyze_scale_factor=$AUTOVACUUM_ANALYZE_SCALE_FACTOR,
        autovacuum_vacuum_cost_delay=0);
EOF
    echo "[postgres_run] cluster up on $PGDATA (profile=$POSTGRES_PROFILE, shared_buffers=$SHARED_BUFFERS, full_page_writes=$actual_full_page_writes, checkpoint_timeout=$CHECKPOINT_TIMEOUT, max_wal_size=$MAX_WAL_SIZE, fillfactor=$TABLE_FILLFACTOR, synchronous_commit=$actual_sync_commit, fsync=$actual_fsync, wal_sync_method=$actual_wal_sync_method)"
    exit 0
    ;;
  cleanup)
    exit 0
    ;;
  load|run)
    if [[ $# -lt 1 ]]; then
      echo "Usage: $0 $MODE <workload>" >&2
      exit 1
    fi
    workload="$1"
    ;;
  end)
    pgctl stop -m fast >/dev/null 2>&1 || true
    if [ -z "${PG_MULTI:-}" ]; then
      :   # tools/apps: no name-based kill (was: pkill -9 -x postgres)
    fi
    exit 0
    ;;
  *)
    echo "Unknown mode: $MODE" >&2
    exit 1
    ;;
esac

# DB-specific overrides (e.g. .fieldcount=...) via DB_SPECIFIC_OPTIONS
declare -A db_opts
for opt in ${DB_SPECIFIC_OPTIONS:-}; do
  db_opts["${opt%%=*}"]="${opt#*=}"
done

COMMON=(
  -P "$YCSB_BASE/workloads/$workload"
  -P "$YCSB_ROOT_BASE/postgrenosql/conf/postgrenosql.properties"
  -p recordcount="$RECORDCOUNT"
  -p postgrenosql.url="jdbc:postgresql://$PG_HOST:$PG_PORT/$PG_DB"
)
for key in "${!db_opts[@]}"; do
  COMMON+=(-p "postgrenosql.$key=${db_opts[$key]}")
done
for opt in ${YCSB_CORE_OPTIONS:-}; do
  COMMON+=(-p "$opt")
done
for opt in ${YCSB_EXTRA_OPTIONS:-}; do
  COMMON+=(-p "$opt")
done

# heap cap per JVM — this ycsb.sh honors JAVA_OPTS (NOT -jvm-args). Keeps 8 loaders + server
# inside the 8G cgroup (Java 11 is cgroup-aware and would otherwise size each heap to 1/4 of 8G).
export JAVA_OPTS="${JAVA_OPTS:-${JVM_ARGS:--Xmx512m}}"

if [[ "$MODE" == "run" ]]; then
  # Time-based runs are bounded by maxexecutiontime; fixed runs use the requested operation count.
  tlimit=()
  run_ops="$OPERATIONCOUNT"
  if [[ -n "${RUN_MAXSECONDS:-}" ]]; then
    run_ops=1000000000
    tlimit=(-p maxexecutiontime="$RUN_MAXSECONDS")
  fi

  if [[ "$YCSB_CLIENT_MODEL" == "threaded" ]]; then
    echo "[postgres_run] threaded run: ${RUN_MAXSECONDS:-fixed-op}s, 1 JVM x $THREADS threads (heap=$JAVA_OPTS)"
    "$YCSB_BIN" run postgrenosql "${COMMON[@]}" \
      -p operationcount="$run_ops" "${tlimit[@]}" -threads "$THREADS" -s
    exit $?
  fi

  if [[ "$YCSB_CLIENT_MODEL" != "multiprocess" ]]; then
    echo "Unknown YCSB_CLIENT_MODEL: $YCSB_CLIENT_MODEL" >&2
    exit 2
  fi

  # Compatibility mode for results produced before the binding became thread-safe.
  RUN_PROCS=${RUN_PROCS:-$THREADS}
  (( RUN_PROCS >= 1 )) || RUN_PROCS=1
  if [[ -n "${RUN_MAXSECONDS:-}" ]]; then
    per=1000000000                      # effectively unlimited; wall-clock bounds each client
    echo "[postgres_run] parallel run: ${RUN_MAXSECONDS}s wall over $RUN_PROCS single-thread clients (heap=$JAVA_OPTS)"
  else
    per=$(( OPERATIONCOUNT / RUN_PROCS ))
    echo "[postgres_run] parallel run: $OPERATIONCOUNT ops over $RUN_PROCS single-thread clients ($per each, heap=$JAVA_OPTS)"
  fi
  tmpd=$(mktemp -d /tmp/pgrun.XXXXXX)
  pids=()
  for (( i=0; i<RUN_PROCS; i++ )); do
    "$YCSB_BIN" run postgrenosql "${COMMON[@]}" \
      -p operationcount="$per" "${tlimit[@]}" -threads 1 -s > "$tmpd/run_$i.log" 2>&1 &
    pids+=($!)
  done
  rc=0; for pid in "${pids[@]}"; do wait "$pid" || rc=1; done
  cat "$tmpd"/run_*.log
  # aggregate: system throughput = total ops / max client runtime; sum error/ok counts
  awk '
    /\[OVERALL\], Throughput/      { sthr+=$NF }
    /\[OVERALL\], RunTime/         { if($NF+0>rt) rt=$NF }
    /\[READ\], Operations/         { rd+=$NF }
    /\[UPDATE\], Operations/       { up+=$NF }
    /\[UPDATE\], Return=OK/        { oku+=$NF }
    /\[UPDATE\], Return=ERROR/     { eru+=$NF }
    /\[READ\], Return=ERROR/       { err+=$NF }
    END {
      printf "[AGGREGATE], clients, %d\n", '"$RUN_PROCS"'
      printf "[AGGREGATE], READ_ops, %d\n", rd
      printf "[AGGREGATE], UPDATE_ops, %d  (OK=%d ERROR=%d)\n", up, oku, eru
      printf "[AGGREGATE], READ_errors, %d\n", err
      printf "[AGGREGATE], MaxRunTime(ms), %d\n", rt
      printf "[OVERALL], Throughput(ops/sec), %.2f\n", sthr
    }' "$tmpd"/run_*.log
  rm -rf "$tmpd"
  exit $rc
fi

# LOAD: this YCSB build does NOT partition insert keys per-thread — all threads in one process
# collide on the same key range, which a PRIMARY KEY makes fatal. Shard by PROCESS instead:
# launch $LOAD_PROCS single-threaded loaders over disjoint insertstart/insertcount ranges.
LOAD_PROCS=${LOAD_PROCS:-8}
(( LOAD_PROCS >= 1 )) || LOAD_PROCS=1
total="$INSERTCOUNT"
chunk=$(( total / LOAD_PROCS ))
echo "[postgres_run] parallel load: $total records over $LOAD_PROCS disjoint ranges (chunk=$chunk, heap=$JAVA_OPTS)"
pids=(); rc=0
for (( i=0; i<LOAD_PROCS; i++ )); do
  start=$(( i * chunk ))
  cnt=$chunk
  if (( i == LOAD_PROCS-1 )); then cnt=$(( total - start )); fi   # last loader absorbs remainder
  "$YCSB_BIN" load postgrenosql "${COMMON[@]}" \
    -p insertstart="$start" -p insertcount="$cnt" -threads 1 -s &
  pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid" || rc=1; done
echo "[postgres_run] parallel load done (rc=$rc)"
exit $rc
