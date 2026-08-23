#!/bin/bash
# Kill-test matrix: zipf x mode. Equal total physical space (OP 9.277% = proposed's 7% + 8 fine blocks @64GiB).
# proposed RAM fixed at 10% of page4k (quota 2.2% = 8 blocks).
# dftl / mu get cache_bytes = proposed's measured RAM (max over zipfs).
cd "$(dirname "$0")/.."
OUT=${1:-results/kill}; shift
mkdir -p "$OUT"
CAP=${CAP:-64G}
ZIPFS=${ZIPFS:-"0 0.8 0.99 1.2"}
OPEQ=0.09278
pids=()
for z in $ZIPFS; do
  ./ftlsim/ftlsim mode=proposed capacity=$CAP zipf=$z tag=proposed out_ts=$OUT/ts_proposed_z$z.csv "$@" > $OUT/proposed_z$z.json 2> $OUT/proposed_z$z.log &
  pids+=($!)
done
for p in "${pids[@]}"; do wait $p; done
MEM=$(python3 -c "import json,glob;print(int(max(json.load(open(f))['mem']['total_bytes'] for f in glob.glob('$OUT/proposed_z*.json'))))")
echo "proposed RAM (max over zipf) = $MEM bytes -> cache_bytes for dftl/mu"
pids=()
for z in $ZIPFS; do
  for m in page4k iu_rmw; do
    ./ftlsim/ftlsim mode=$m capacity=$CAP zipf=$z op=0.07 tag=${m}_op7 "$@" > $OUT/${m}_op7_z$z.json 2> $OUT/${m}_op7_z$z.log &
    pids+=($!)
  done
  for m in page4k iu_rmw fast last; do
    ./ftlsim/ftlsim mode=$m capacity=$CAP zipf=$z op=$OPEQ tag=${m}_eq "$@" > $OUT/${m}_eq_z$z.json 2> $OUT/${m}_eq_z$z.log &
    pids+=($!)
  done
  for m in dftl mu; do
    ./ftlsim/ftlsim mode=$m capacity=$CAP zipf=$z op=$OPEQ cache_bytes=$MEM tag=${m}_eq "$@" > $OUT/${m}_eq_z$z.json 2> $OUT/${m}_eq_z$z.log &
    pids+=($!)
  done
done
fail=0
for p in "${pids[@]}"; do wait $p || fail=1; done
[ $fail = 1 ] && echo "some runs failed; check $OUT/*.log"
python3 tools/summarize.py "$OUT"
