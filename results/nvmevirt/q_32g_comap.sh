#!/bin/bash
# (2026-11-13, user) CoMap configs that may do better, after q_32g_ns.sh: comap_g32p1k (agent period 1024, learning on)
# and comap_g32pg (P fixed at 0.99, learning off; best WAF in results/comap_sim/tune). All 9 workloads.
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
# (2026-11-13) run sequentially from q_32g_after_reboot.sh (old wait on PID 34555 removed)
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g/comap_tune; D=/mnt/nvme_mounted/dm_trace_32g; mkdir -p $R
. tools/apps/common.sh >/dev/null 2>&1
for cfg in comap_g32p1k comap_g32pg; do
  P="comap:$(policy_params $cfg)"
  for t in a128_r50_s70 a128_r70_s70 a128_r90_s70; do [ -s $R/$cfg/replay_$t/comap/result.json ] && continue; idle; echo "$(date) $cfg $t"
    TRACE=$t TRACE_DIR=$D RESULTS=$R/$cfg/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$P"; done
  for t in ali3 spfresh; do [ -s $R/$cfg/trace_$t/comap/result.json ] && continue; idle; echo "$(date) $cfg $t"
    TRACE=$t TRACE_DIR=$D RESULTS=$R/$cfg/trace_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$P"; done
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 tpcc_d_t64" POLICIES="comap_g32p1k comap_g32pg" \
  tools/apps/run_all.sh >> $R/apps.run_all.log 2>&1
echo "$(date) all done"
