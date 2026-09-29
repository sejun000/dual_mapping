#!/bin/bash
# Real-trace replay at equal total physical space (OP 9.278%), canonical v3.2 operating points.
#   tools/run_trace.sh <packed.csv from trace_prep> <capacity_bytes> <outdir> [extra key=value ...]
# DFTL and LeaFTL run after the proposed points, with its CMT sized to each point's measured RAM and the
# last-read translation page buffered in SRAM (dftl_tpbuf=1: no per-entry flash read for sequential runs).
cd "$(dirname "$0")/.."
TR=$1; CAP=$2; OUT=$3; shift 3
mkdir -p "$OUT"
OPEQ=${OPEQ:-0.09278}
SIM=./ftlsim/ftlsim
COM="trace=$TR capacity=$CAP"
run() { local tag=$1; shift; $SIM $COM tag=$tag "$@" > $OUT/$tag.json 2> $OUT/$tag.log; }
run prop_lo mode=proposed op=0.07 fine_extra=1 fine_quota=0.022 cgc_vict=cb coarse_class=2 "$@" &
run prop_hi mode=proposed U=8 op=$OPEQ fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 "$@" &
run page4k mode=page4k op=$OPEQ "$@" &
run sepbit mode=sepbit op=$OPEQ "$@" &
run iu_rmw mode=iu_rmw op=$OPEQ "$@" &
wait
for p in lo hi; do
  MEM=$(python3 -c "import json;print(int(json.load(open('$OUT/prop_$p.json'))['mem']['total_bytes']))")
  run dftl_$p mode=dftl op=$OPEQ cache_bytes=$MEM dftl_tpbuf=1 "$@" &
  run leaftl_$p mode=leaftl op=$OPEQ cache_bytes=$MEM "$@" &
done
wait
python3 - "$OUT" <<'PY'
import json, glob, sys, os
rows = []
for f in sorted(glob.glob(sys.argv[1] + '/*.json')):
    try: d = json.load(open(f))
    except Exception: print('FAILED', f); continue
    rows.append((d['tag'], d['NB'], d['WAF'], d['mem']['total_bytes'] / 2**20, d['reads'].get('map', 0) / max(1, d['host_pages'])))
for t, nb, w, m, mm in sorted(rows, key=lambda r: r[2]):
    print(f"{t:10s} NB={nb:4d} WAF={w:7.3f} RAM={m:8.1f} MiB map_reads/page={mm:.3f}")
PY
