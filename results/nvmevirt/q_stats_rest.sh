#!/bin/bash
# (2026-11-06) Fine-area stats of the final DUET on the remaining six workloads (raw I/O at 4K:128K = 70:30 only) (SPFresh, Ali3, YCSB-A, R50 70:30 are in
# results/final_stats already). Same module and params as q_stats3.sh. Starts after q_fastfull.sh and q_ts.sh. -> results/final_stats
cd /home/sejun000/dual_mapping
while pgrep -f "^/bin/bash (/home/sejun000/dual_mapping/)?results/nvmevirt/q_(fastfull|ts)\.sh" >/dev/null; do sleep 60; done
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_stats; mkdir -p $R
DU="U=16,fine_extra=0,fine_quota_ppm=48300,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=0,adapt=2,uni_onefront=0,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125476,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"
# (apps done 2026-11-06 16:37, rerun of the raw I/O part only after the set/-- typo fix)
for cfg in "70 70" "90 70"; do   # 4K:128K = 70:30 only
  set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "proposedst:$DU"
done
echo "$(date) all done"
