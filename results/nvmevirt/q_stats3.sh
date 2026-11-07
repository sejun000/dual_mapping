#!/bin/bash
# (2026-11-06) Fine-area stats: final DUET (= q_vbm2.sh, cost-benefit coarse victim) on ~/nvmevirt_proposedst, which only adds
# fine_blocks / fpages / spages (+ running sums) to every [dm ...] line. Decisions unchanged. Same 13 workloads.
# Reduced to the three ablation workloads (YCSB-A, Ali3, R50 70:30). Starts after q_ablation.sh. -> results/final_stats
cd /home/sejun000/dual_mapping
while pgrep -f "^/bin/bash (/home/sejun000/dual_mapping/)?results/nvmevirt/q_ablation\.sh" >/dev/null; do sleep 60; done
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_stats; mkdir -p $R
DU="U=16,fine_extra=0,fine_quota_ppm=48300,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=0,adapt=2,uni_onefront=0,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125476,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"
D=/mnt/nvme_mounted/dm_trace_sampled
geom() { CAP=$(python3 -c "import json;print(json.load(open('$D/$1.json'))['capacity_bytes'])"); python3 tools/nvmev_geom.py --fit $CAP; }
for t in ali3; do
  idle; M=$(geom $t); echo "$(date) trace $t memmap_size=$M"
  TRACE=$t RESULTS=$R/trace_$t NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposedst:$DU"
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64" \
  POLICIES="proposedst_vb" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
for cfg in "50 70"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "proposedst:$DU"
done
echo "$(date) all done"
