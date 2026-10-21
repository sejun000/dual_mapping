#!/bin/bash
# Harness <-> ftlsim parity on the validation traces.
#   validate.sh <policy> "<ftlsim key=value ...>" ["<extra harness key=value ...>"]
# e.g. validate.sh page4k "mode=page4k op=0.09278"
#      validate.sh dftl   "mode=dftl op=0.09278 cache_bytes=400K dftl_tpbuf=1" "'params=cache_bytes=409600 dftl_tpbuf=1'"
# Harness args are eval'd: quote a multi-option params= with single quotes (or separate options with commas).
# Geometry must match: the harness uses op=0.09278 and ftlsim's auto P unless overridden (pass P=/U=/op= to both).
# Also compared: flash map programs / map reads (prog.map, reads.map). READS=1 replays the traces' host reads too
# (ftlsim dram_stats=1, harness reads=1; ftlsim only replays reads with dram_stats). TRACES overrides the trace list
# ("name:capacity[:file]" words). A run that produces no JSON is reported as FAILED, never as a match.
set -u
POL=$1; SIMARGS=$2; HARGS=${3:-}
H=$(cd "$(dirname "$0")" && pwd); SIM=${SIM:-/home/sejun000/dual_mapping/ftlsim/ftlsim}
TR=${TR:-/mnt/nvme_mounted/dm_validate}
mkdir -p $TR
[ -s $TR/skew.csv ] || python3 $H/gen_trace.py skew $TR/skew.csv >/dev/null
[ -s $TR/rand4k.csv ] || python3 $H/gen_trace.py rand4k $TR/rand4k.csv >/dev/null
(cd $H && make -s POLICY=$POL) || exit 1
fail=0
READS=${READS:-0}; SR=""; HR=""; [ "$READS" = 1 ] && { SR="dram_stats=1"; HR="reads=1"; }
for t in ${TRACES:-skew:8G rand4k:8G kvsmoke:65694334976}; do
  name=${t%%:*}; rest=${t#*:}; cap=${rest%%:*}; f=${rest#*:}; [ "$f" = "$rest" ] && f=$TR/$name.csv
  [ $name = kvsmoke ] && [ "$f" = "$TR/$name.csv" ] && f=/mnt/nvme_mounted/kvtrace/smoke.packed.csv
  a=$($SIM trace=$f capacity=$cap $SIMARGS $SR 2>/dev/null | python3 -c "import json,sys;d=json.load(sys.stdin);print(d['WAF'],d['total_prog'],d['erases'],d['prog'].get('map',0),d['reads'].get('map',0))" 2>/dev/null)
  b=$(eval "\"$H/replay_$POL\" trace=$f capacity=$cap $HARGS $HR" 2>/dev/null | python3 -c "import json,sys;d=json.load(sys.stdin);print(d['WAF'],d['total_prog'],d['erases'],d['prog']['map'],d['read']['map'],d['order_violations'])" 2>/dev/null)
  echo "$name  ftlsim(WAF prog erases map_prog map_read)=[$a]  harness(WAF prog erases map_prog map_read order_viol)=[$b]"
  [ -n "$a" ] || { echo "  FAILED: ftlsim produced no JSON (run: $SIM trace=$f capacity=$cap $SIMARGS $SR)"; fail=1; }
  [ -n "$b" ] || { echo "  FAILED: harness produced no JSON"; fail=1; }
  [ -n "$a" ] && [ "${a}" = "$(echo $b | awk '{print $1,$2,$3,$4,$5}')" ] && [ "$(echo $b | awk '{print $6}')" = 0 ] || fail=1
done
[ $fail = 0 ] && echo "PARITY: bit-identical" || echo "PARITY: MISMATCH"
