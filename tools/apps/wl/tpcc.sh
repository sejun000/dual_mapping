# wl/tpcc.sh - splitEval workloads/tpcc_multi.sh as configured by tests/mn_campaign_queue.sh (tpcc320):
#   WORKLOAD=tpcc-multi DURATION=600 FILL_TARGET_PERCENT=75(ext4: df 76) SYNC_COMMIT=on TPCCM_INSTANCES=16 WAREHOUSES=32
#   TERMINALS=320 MAX_CONNECTIONS=64
# plus the plugin defaults (READ_COMMITTED, batch 128, rate 100000, weights 45/43/4/4/4, open_datasync,
# full_page_writes on, shared_buffers 256MB, checkpoint_timeout 30s, bgwriter 10ms/4000/10.0, max_wal 512MB,
# min_wal 80MB, autovacuum 2 workers / 10s). Each PostgreSQL runs in its own systemd unit with
# MemoryMax=1G / MemoryHigh=896M under numactl, nested in the common dmapp.slice; BenchBase JVMs (-Xmx512m) run
# through $SCOPE, also in dmapp.slice (the study gave BenchBase no cgroup).

WL_KIND=tpcc-multi
WL_SELF_CAPTURE=0
WL_FILL="${FILL_TARGET_PERCENT:-76}"   # ext4: df Use% target = the study's PostgreSQL start utilisation (76%)
UTIL_MIN="${UTIL_MIN:-$((WL_FILL - 3))}"; UTIL_MAX="${UTIL_MAX:-85}"   # the load alone may land above 76%
DURATION="${DURATION:-600}"
TPCCM_INSTANCES="${TPCCM_INSTANCES:-16}"
WAREHOUSES="${WAREHOUSES:-32}"
TERMINALS="${TERMINALS:-320}"
RUNTIME="${RUNTIME:-$DURATION}"
RATE="${RATE:-100000}"
ISOLATION="${ISOLATION:-TRANSACTION_READ_COMMITTED}"
BATCHSIZE="${BATCHSIZE:-128}"
SYNC_COMMIT="${SYNC_COMMIT:-on}"
POSTGRES_FSYNC="${POSTGRES_FSYNC:-on}"
POSTGRES_WAL_SYNC_METHOD="${POSTGRES_WAL_SYNC_METHOD:-open_datasync}"
FULL_PAGE_WRITES="${FULL_PAGE_WRITES:-on}"
SHARED_BUFFERS="${SHARED_BUFFERS:-256MB}"
CHECKPOINT_TIMEOUT="${CHECKPOINT_TIMEOUT:-30s}"
BGWRITER_DELAY="${BGWRITER_DELAY:-10ms}"
BGWRITER_LRU_MAXPAGES="${BGWRITER_LRU_MAXPAGES:-4000}"
BGWRITER_LRU_MULTIPLIER="${BGWRITER_LRU_MULTIPLIER:-10.0}"
MAX_WAL_SIZE="${MAX_WAL_SIZE:-512MB}"
MIN_WAL_SIZE="${MIN_WAL_SIZE:-80MB}"
MAX_CONNECTIONS="${MAX_CONNECTIONS:-64}"
TPCCM_JAVA_HEAP="${TPCCM_JAVA_HEAP:-512m}"
TPCCM_MEMORY_MAX="${TPCCM_MEMORY_MAX:-1G}"
TPCCM_MEMORY_HIGH="${TPCCM_MEMORY_HIGH:-896M}"

PG_BIN=/usr/lib/postgresql/16/bin
JAVA21=/usr/lib/jvm/java-21-openjdk-amd64/bin/java
BENCHBASE="${BENCHBASE:-/home/sejun000/splitFTL/gyjin/distfiles/tpcc-postgres/target/benchbase-postgres}"
TPCCM_BASE="$MNT/postgresql_tpccmulti"
PG_HOST=127.0.0.1
TPCCM_PORT_BASE="${TPCCM_PORT_BASE:-5433}"
PG_DB=benchbase
PGLOG_DIR=/tmp/dmapp_pg
BB_PIDS=()

tpccm_port() { echo $((TPCCM_PORT_BASE + $1 - 1)); }
tpccm_data() { echo "$TPCCM_BASE/$1/main"; }
tpccm_unit() { echo "dmapp_pg_tpccm_$1"; }
tpccm_wh()   { local w=$((WAREHOUSES / TPCCM_INSTANCES)); ((w >= 1)) || w=1; echo "$w"; }
tpccm_term() { local t=$((TERMINALS / TPCCM_INSTANCES)); ((t >= 1)) || t=1; echo "$t"; }
psql_i() { local i="$1"; shift; sudo -u postgres "$PG_BIN/psql" -h "$PG_HOST" -p "$(tpccm_port "$i")" -v ON_ERROR_STOP=1 "$@"; }

stop_postgres_all() {
  local i
  for i in $(seq "$TPCCM_INSTANCES"); do
    sudo systemctl stop "$(tpccm_unit "$i").service" 2>/dev/null || true
    sudo -u postgres "$PG_BIN/pg_ctl" -D "$(tpccm_data "$i")" stop -m immediate >/dev/null 2>&1 || true
  done
}

wl_check_deps() {
  local path i p
  for path in "$PG_BIN/initdb" "$JAVA21" "$BENCHBASE/benchbase.jar"; do [[ -e "$path" ]] || die "missing $path"; done
  (( $(tpccm_wh) <= $(tpccm_term) )) || die "BenchBase needs terminals >= warehouses per instance"
  for i in $(seq "$TPCCM_INSTANCES"); do p=$(tpccm_port "$i")
    port_in_use "$p" && die "port $p already in use"; done; return 0
}

tpccm_config() {
  local i="$1" path="$CASE_DIR/tpcc_$i.xml"
  cat > "$path" <<XML
<?xml version="1.0"?>
<parameters>
    <type>POSTGRES</type>
    <driver>org.postgresql.Driver</driver>
    <url>jdbc:postgresql://$PG_HOST:$(tpccm_port "$i")/$PG_DB?sslmode=disable&amp;ApplicationName=tpcc&amp;reWriteBatchedInserts=true</url>
    <username>postgres</username>
    <password>password</password>
    <isolation>${ISOLATION}</isolation>
    <batchsize>${BATCHSIZE}</batchsize>
    <maxConnections>${MAX_CONNECTIONS}</maxConnections>
    <scalefactor>$(tpccm_wh)</scalefactor>
    <terminals>$(tpccm_term)</terminals>
    <works>
        <work><time>${RUNTIME}</time><rate>${RATE}</rate><weights>45,43,4,4,4</weights></work>
    </works>
    <transactiontypes>
        <transactiontype><name>NewOrder</name></transactiontype>
        <transactiontype><name>Payment</name></transactiontype>
        <transactiontype><name>OrderStatus</name></transactiontype>
        <transactiontype><name>Delivery</name></transactiontype>
        <transactiontype><name>StockLevel</name></transactiontype>
    </transactiontypes>
</parameters>
XML
  echo "$path"
}

tpccm_start_one() {
  local i="$1" pgdata port unit plog
  pgdata="$(tpccm_data "$i")"; port="$(tpccm_port "$i")"; unit="$(tpccm_unit "$i")"; plog="$PGLOG_DIR/pg_tpccm_$i.log"
  sudo rm -rf "$pgdata"; sudo mkdir -p "$pgdata"
  sudo chown -R postgres:postgres "$TPCCM_BASE/$i"; sudo chmod 700 "$pgdata"
  sudo -u postgres "$PG_BIN/initdb" -D "$pgdata" -E UTF8 --no-locale >/dev/null
  sudo -u postgres tee -a "$pgdata/postgresql.conf" >/dev/null <<EOF

# ===== TPC-C multi-instance profile (instance $i/$TPCCM_INSTANCES) =====
listen_addresses = '$PG_HOST'
port = $port
shared_buffers = $SHARED_BUFFERS
synchronous_commit = $SYNC_COMMIT
full_page_writes = $FULL_PAGE_WRITES
wal_sync_method = '$POSTGRES_WAL_SYNC_METHOD'
checkpoint_timeout = $CHECKPOINT_TIMEOUT
bgwriter_delay = $BGWRITER_DELAY
bgwriter_lru_maxpages = $BGWRITER_LRU_MAXPAGES
bgwriter_lru_multiplier = $BGWRITER_LRU_MULTIPLIER
max_wal_size = $MAX_WAL_SIZE
min_wal_size = $MIN_WAL_SIZE
fsync = $POSTGRES_FSYNC
max_connections = $MAX_CONNECTIONS
unix_socket_directories = '/tmp'
autovacuum = on
autovacuum_max_workers = 2
autovacuum_naptime = 10s
EOF
  sudo -u postgres tee "$pgdata/pg_hba.conf" >/dev/null <<EOF
local   all   all                 trust
host    all   all   127.0.0.1/32  trust
host    all   all   ::1/128       trust
EOF
  sudo rm -f "$plog"
  sudo systemctl reset-failed "$unit.service" 2>/dev/null || true
  sudo systemd-run --unit="$unit" --collect --slice="$MEMCG_SLICE" \
    --property=MemoryMax="$TPCCM_MEMORY_MAX" --property=MemoryHigh="$TPCCM_MEMORY_HIGH" \
    --property=User=postgres --property=Group=postgres \
    --property=StandardOutput=append:"$plog" --property=StandardError=append:"$plog" \
    $NUMACTL "$PG_BIN/postgres" -D "$pgdata" >/dev/null 2>&1
  local up=0
  for _ in $(seq 120); do "$PG_BIN/pg_isready" -h "$PG_HOST" -p "$port" >/dev/null 2>&1 && { up=1; break; }; sleep 1; done
  [[ "$up" == 1 ]] || { echo "[FATAL] instance $i did not start" >&2; sudo tail -20 "$plog" >&2; return 1; }
  psql_i "$i" -d postgres >/dev/null <<EOF
DROP DATABASE IF EXISTS $PG_DB;
CREATE DATABASE $PG_DB;
EOF
  local work="$CASE_DIR/bb_$i"; mkdir -p "$work"
  (cd "$BENCHBASE" && $SCOPE $NUMACTL "$JAVA21" -Xmx"$TPCCM_JAVA_HEAP" -jar benchbase.jar \
      -b tpcc -c "$(tpccm_config "$i")" --create=true --load=true --execute=false -d "$work") \
      > "$CASE_DIR/tpcc_load_$i.log" 2>&1
}

wl_setup() {
  local i pids=() failed=0
  mkdir -p "$PGLOG_DIR"; sudo chmod 1777 "$PGLOG_DIR"; sudo rm -f "$PGLOG_DIR"/pg_tpccm_*.log
  stop_postgres_all
  sudo rm -rf "$TPCCM_BASE"; sudo mkdir -p "$TPCCM_BASE"; sudo chown postgres:postgres "$TPCCM_BASE"
  log "[tpcc] $TPCCM_INSTANCES instances x ($(tpccm_wh) warehouses, $(tpccm_term) terminals)"
  for i in $(seq "$TPCCM_INSTANCES"); do tpccm_start_one "$i" & pids+=($!); done
  for i in "${!pids[@]}"; do wait "${pids[$i]}" || failed=1; done
  ((failed)) && die "TPC-C load failed (see tpcc_load_*.log)"
  return 0
}

wl_post_ballast() {
  local i; for i in $(seq "$TPCCM_INSTANCES"); do psql_i "$i" -d "$PG_DB" -c 'CHECKPOINT;' >/dev/null; done
}

wl_run() {
  local i pids=()
  log "[tpcc] execute ${RUNTIME}s, $TPCCM_INSTANCES x $(tpccm_term) terminals"
  WL_STATUS=0
  for i in $(seq "$TPCCM_INSTANCES"); do
    (cd "$BENCHBASE" && $SCOPE $NUMACTL "$JAVA21" -Xmx"$TPCCM_JAVA_HEAP" -jar benchbase.jar \
        -b tpcc -c "$CASE_DIR/tpcc_$i.xml" --create=false --load=false --execute=true \
        -d "$CASE_DIR/bb_$i") > "$CASE_DIR/tpcc_run_$i.log" 2>&1 & pids+=($!)
  done
  BB_PIDS=("${pids[@]}")
  for i in "${!pids[@]}"; do wait "${pids[$i]}" || WL_STATUS=$?; done
  BB_PIDS=()
}

wl_ok() {
  local i; for i in $(seq "$TPCCM_INSTANCES"); do compgen -G "$CASE_DIR/bb_$i/*.summary.json" >/dev/null || return 1; done
}

wl_teardown() { local p; for p in "${BB_PIDS[@]}"; do kill_own "$p"; done; BB_PIDS=(); stop_postgres_all; sudo cp "$PGLOG_DIR"/pg_tpccm_*.log "$CASE_DIR/" 2>/dev/null && sudo chown "$USER" "$CASE_DIR"/pg_tpccm_*.log; true; }

wl_config_lines() {
  printf 'instances=%s\nwarehouses_total=%s\nterminals_total=%s\nbatchsize=%s\nisolation=%s\nruntime=%s\nrate=%s\n' \
    "$TPCCM_INSTANCES" "$WAREHOUSES" "$TERMINALS" "$BATCHSIZE" "$ISOLATION" "$RUNTIME" "$RATE"
  printf 'synchronous_commit=%s\nfsync=%s\nwal_sync_method=%s\nfull_page_writes=%s\nshared_buffers=%s\n' \
    "$SYNC_COMMIT" "$POSTGRES_FSYNC" "$POSTGRES_WAL_SYNC_METHOD" "$FULL_PAGE_WRITES" "$SHARED_BUFFERS"
  printf 'checkpoint_timeout=%s\nmax_wal_size=%s\nmin_wal_size=%s\nmax_connections=%s\n' \
    "$CHECKPOINT_TIMEOUT" "$MAX_WAL_SIZE" "$MIN_WAL_SIZE" "$MAX_CONNECTIONS"
  printf 'bgwriter_delay=%s\nbgwriter_lru_maxpages=%s\nbgwriter_lru_multiplier=%s\n' \
    "$BGWRITER_DELAY" "$BGWRITER_LRU_MAXPAGES" "$BGWRITER_LRU_MULTIPLIER"
  printf 'java_heap=%s\npg_memory_max=%s\npg_memory_high=%s\n' "$TPCCM_JAVA_HEAP" "$TPCCM_MEMORY_MAX" "$TPCCM_MEMORY_HIGH"
  printf 'postgres_version=%s\njava_version=%s\n' "$("$PG_BIN/postgres" --version)" "$("$JAVA21" -version 2>&1 | head -1)"
}
