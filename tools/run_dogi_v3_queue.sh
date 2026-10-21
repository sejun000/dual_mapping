#!/bin/bash
# Run tools/run_dogi_v3.sh for several workloads of results/dogi_v3/jobs.txt in parallel.
#   tools/run_dogi_v3_queue.sh <modes> <xtag> <jobs> <workload...>
#   e.g. tools/run_dogi_v3_queue.sh strict _sh 5 ali3 ali2 ali1 ssd z0 ...
#        DOGI_GUARD_ANY=1 tools/run_dogi_v3_queue.sh strict _se 3 ...     (DOGI+)
#        DOGI_GUARD=1     tools/run_dogi_v3_queue.sh gp _gh 4 ...         (artifact accounting + guard)
cd "$(dirname "$0")/.."
export MODES=$1 XTAG=$2; J=$3; shift 3
for w in "$@"; do
  IFS=$'\t' read -r name P NB args < <(grep -P "^\\Q$w\\E\t" results/dogi_v3/jobs.txt)
  printf '%s\0%s\0%s\0%s\0' "$name" "$P" "$NB" "$args"
done | xargs -0 -n 4 -P "$J" tools/run_dogi_v3.sh
