#!/bin/bash
# (2026-10-30) Ablation: DUET (proposed_b12q, spill to NAND) + standard cuckoo displacement before a spill
# (ct_kick=500: BFS over <= 500 buckets, alias proposed_b12qk) on the paper workload set:
# SPFresh + Ali-3 traces, YCSB-A/B/C/E + TPC-C (aged, PREFILL_SEQ=1), raw 128K-aligned QD32 replays R50/R70/R90 x 7:3 / 3:7.
# -> results/final_kick; compare with results/final_q18 (same config without ct_kick).
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_kick; mkdir -p $R
V="U=16,fine_extra=0,fine_quota_ppm=48300,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
S="proposed:$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1,ct_kick=500"
D=/mnt/nvme_mounted/dm_trace_sampled
for TRACE in spfresh ali3; do
  idle; CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $TRACE memmap_size=$M"
  TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$S"
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" POLICIES="proposed_b12qk" \
  tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
for cfg in "50 70" "70 70" "90 70" "50 30" "70 30" "90 30"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$S"
done
echo "$(date) all done"
