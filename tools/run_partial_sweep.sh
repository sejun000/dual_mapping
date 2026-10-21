#!/bin/bash
# Fill-fraction x skew sweep at the NVMeVirt 44G geometry (tools/run_partial.sh policies, one job per run,
# at most $JOBS concurrent). Fresh device: seq fill of fill_frac, workload over foot_frac = fill_frac - 0.03.
# Aged device (user decision 2026-10-08): full sequential fill (every LBA mapped, no TRIM), then the same
# ext4-like partial use (workload over foot_frac=0.73). Requests 4-16 KiB (min_k=0 max_k=2).
#   JOBS=30 POLS="V3 V3F page4k dftl leaftl" tools/run_partial_sweep.sh results/partial/sweep
cd "$(dirname "$0")/.."
OUT=${1:-results/partial/sweep}; JOBS=${JOBS:-30}
POLS=${POLS:-V3 V3F page4k dftl leaftl}
{
for z in 0 0.8 0.99 1.2; do
  for f in 0.5 0.6 0.76 0.9 1.0; do
    ft=$(python3 -c "print(round($f-0.03,4) if $f<1 else 1)")
    for p in $POLS; do echo "$OUT/fresh_f${f}_z$z $p fill_frac=$f foot_frac=$ft zipf=$z min_k=0 max_k=2"; done
  done
  for p in $POLS; do echo "$OUT/aged_f0.76_z$z $p fill_frac=1 foot_frac=0.73 zipf=$z min_k=0 max_k=2"; done
done
} | xargs -P $JOBS -L 1 bash -c 'd=$0; p=$1; shift 1; POLS=$p tools/run_partial.sh $d "$@"'
