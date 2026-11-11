#!/bin/bash
# (2026-11-10) Coarse-IU sensitivity on the 50:50 raw I/O (a128_r50_s70), so Fig. 10 uses the same raw I/O as the
# ablation (Fig. 8). Same build and params as q_usens.sh. U = 16 is results/final_vbm. -> results/final_usens/u<U>/replay_a128_r50_s70
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_usens
. tools/apps/common.sh >/dev/null 2>&1
t=a128_r50_s70
for u in 128 64 32; do
  P=$(policy_params proposed_vbu$u)
  idle; echo "$(date) U=$u replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/u$u/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposed:$P"
done
echo "$(date) all done"
