#!/bin/bash
# (2026-11-13) app part of q_32g_abl2.sh, rerun: the first try failed at 07:47 because ~/nvmevirt_page4k (B18 probe) was
# still built for kernel 6.8.0-142. Rebuilt for 6.8.0-149.
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g/ablation2
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64" \
  POLICIES="proposedab_g32bk proposedab_g32bs proposedab_g32bsc" tools/apps/run_all.sh >> $R/apps.run_all.log 2>&1
echo "$(date) all done"
