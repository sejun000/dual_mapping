#!/bin/bash
# (2026-11-11) uFTL (mu-FTL) on the 32 GB device, after results/nvmevirt/q_32g.sh and q_32g_rest.sh, q_32g_ab.sh: same workloads as the main matrix.
# ~/nvmevirt_mu rebuilt with BLKS_PER_PLN 274; budget mu_g32 = T - NB*P/8 - 32768 - NB = 3180470 B (rule of mu_vb).
cd /home/sejun000/dual_mapping
while pgrep -f '^(/bin/)?bash (/home/sejun000/dual_mapping/)?results/nvmevirt/q_32g(_rest|_ab)?\.sh' >/dev/null; do sleep 60; done
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g; D=/mnt/nvme_mounted/dm_trace_32g
. tools/apps/common.sh >/dev/null 2>&1
MU="mu:$(policy_params mu_g32)"
for t in a128_r50_s70 a128_r70_s70 a128_r90_s70; do idle; echo "$(date) $t mu"
  TRACE=$t TRACE_DIR=$D RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$MU"; done
for t in ali3 spfresh; do [ -e $D/$t.csv ] || continue; idle; echo "$(date) $t mu"
  TRACE=$t TRACE_DIR=$D RESULTS=$R/trace_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$MU"; done
idle; echo "$(date) apps mu"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 tpcc_d_t64" POLICIES="mu_g32" \
  tools/apps/run_all.sh >> $R/apps.run_all.log 2>&1
echo "$(date) all done"
