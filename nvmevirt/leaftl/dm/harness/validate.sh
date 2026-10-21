#!/bin/bash
# Harness <-> ftlsim parity on the validation traces.
#   validate.sh <policy> "<ftlsim key=value ...>" ["<extra harness key=value ...>"]
# e.g. validate.sh page4k "mode=page4k op=0.09278"
#      validate.sh dftl   "mode=dftl op=0.09278 cache_bytes=400K dftl_tpbuf=1" "'params=cache_bytes=409600 dftl_tpbuf=1'"
# Harness args are eval'd: quote a multi-option params= with single quotes (or separate options with commas).
# Geometry must match: the harness uses op=0.09278 and ftlsim's auto P unless overridden (pass P=/U=/op= to both).
set -u
POL=$1; SIMARGS=$2; HARGS=${3:-}
H=$(cd "$(dirname "$0")" && pwd); SIM=${SIM:-/home/sejun000/dual_mapping/ftlsim/ftlsim}
TR=${TR:-/mnt/nvme_mounted/dm_validate}
mkdir -p $TR
[ -s $TR/skew.csv ] || python3 $H/gen_trace.py skew $TR/skew.csv >/dev/null
[ -s $TR/rand4k.csv ] || python3 $H/gen_trace.py rand4k $TR/rand4k.csv >/dev/null
(cd $H && make -s POLICY=$POL) || exit 1
fail=0
for t in skew:8G rand4k:8G kvsmoke:65694334976; do
  name=${t%%:*}; cap=${t#*:}
  f=$TR/$name.csv; [ $name = kvsmoke ] && f=/mnt/nvme_mounted/kvtrace/smoke.packed.csv
  a=$($SIM trace=$f capacity=$cap $SIMARGS 2>/dev/null | python3 -c "import json,sys;d=json.load(sys.stdin);print(d['WAF'],d['total_prog'],d['erases'])")
  b=$(eval "\"$H/replay_$POL\" trace=$f capacity=$cap $HARGS" 2>/dev/null | python3 -c "import json,sys;d=json.load(sys.stdin);print(d['WAF'],d['total_prog'],d['erases'],d['order_violations'])")
  echo "$name  ftlsim(WAF prog erases)=[$a]  harness(WAF prog erases order_viol)=[$b]"
  [ "${a}" = "$(echo $b | awk '{print $1,$2,$3}')" ] || fail=1
done
[ $fail = 0 ] && echo "PARITY: bit-identical" || echo "PARITY: MISMATCH"
