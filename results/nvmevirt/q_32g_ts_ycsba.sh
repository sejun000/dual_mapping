#!/bin/bash
# (2026-11-18, user) Fig. 9 with YCSB-A instead of SPFresh: DUET stats build (proposedst_g32 = proposed_g32 params, prints fine_blocks/fpages/spages; the main build does not) on YCSB-A with
# the fine-area size sampled every 5 s via the dm_dump trigger (same poll as q_32g_rest.sh). -> results/final_32g/ts/apps
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
poll() { while true; do
    if [ -e /sys/module/nvmev/parameters/dm_dump ]; then
      echo 1 | sudo tee /sys/module/nvmev/parameters/dm_dump >/dev/null 2>&1
      l=$(sudo dmesg | grep '\[dm flush\].*smp_n=' | tail -1); [ -n "$l" ] && echo "$(date +%s) $l" >> "$1"
    fi; sleep 5; done; }
R=/home/sejun000/dual_mapping/results/final_32g/ts/apps
idle; echo "$(date) start"
mkdir -p $R/ycsba_d_t64
poll $R/ycsba_d_t64/ts.log & PP=$!
PREFILL_SEQ=1 RESULTS_ROOT=$R WORKLOADS="ycsba_d_t64" POLICIES="proposedst_g32" tools/apps/run_all.sh >> $R/run_all.log 2>&1
kill $PP; wait $PP 2>/dev/null
echo "$(date) all done"
