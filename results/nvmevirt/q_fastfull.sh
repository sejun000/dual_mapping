#!/bin/bash
# (2026-11-06) FAST with all spare blocks as log blocks: cache_bytes=1e9 never caps rwcap, so nlog-1 random log blocks
# (apps: 22 instead of 14 under B12). Its log map then exceeds the 1/8 DRAM budget (reference point only).
# Same 13 workloads and traces as final_rp / final_rp_a128 (raw I/O uses the *_fast traces sized to FAST's capacity).
# Starts after q_stats3.sh. -> results/final_fastfull
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_fastfull; mkdir -p $R
F="fast:cache_bytes=1000000000"
D=/mnt/nvme_mounted/dm_trace_sampled
# (relaunched 12:2x UTC: traces done and the apps run_all started by the first launch keeps running; raw I/O reduced to
#  4K:128K = 70:30, the only mix kept in the paper)
for cfg in "50 70" "70 70" "90 70"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay ${t}_fast"
  TRACE=${t}_fast TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$F"
done
echo "$(date) all done"
