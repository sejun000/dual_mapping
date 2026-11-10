#!/bin/bash
# (2026-11-09) Coarse-IU size sensitivity of DUET: U = 32, 64, 128 pages (128 / 256 / 512 KB coarse IU) with the final
# DUET parameters (proposed_vb) and the same map_budget_ppm (the coarse map shrinks as 1/U, the fine table gets the rest).
# U = 16 is results/final_vbm. ~/nvmevirt_proposed: 128-bit fragment / write-buffer masks, prop_fs scratch off the stack,
# glue rounds L up for U > 16 (raw I/O traces span the U=16 L); U=16 harness runs bit-identical to the old build
# (backup: scratchpad u128/). Workloads: 70:30 raw I/O, Alibaba, SPFresh, YCSB-A, TPC-C. -> results/final_usens/u<U>/...
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_usens; mkdir -p $R
. tools/apps/common.sh >/dev/null 2>&1
D=/mnt/nvme_mounted/dm_trace_sampled
for u in 128 64 32; do
  P=$(policy_params proposed_vbu$u)
  t=a128_r70_s70
  idle; echo "$(date) U=$u replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/u$u/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposed:$P"
  for t in ali3 spfresh; do
    idle; CAP=$(python3 -c "import json;print(json.load(open('$D/$t.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
    echo "$(date) U=$u trace $t memmap_size=$M"
    TRACE=$t RESULTS=$R/u$u/trace_$t NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposed:$P"
  done
  idle; echo "$(date) U=$u apps"
  PREFILL_SEQ=1 RESULTS_ROOT=$R/u$u/apps WORKLOADS="ycsba_d_t64 tpcc_d_t64" POLICIES="proposed_vbu$u" \
    tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
done
echo "$(date) all done"
