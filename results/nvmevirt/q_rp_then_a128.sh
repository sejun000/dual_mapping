#!/bin/bash
# (2026-10-19) 1) proposed + spill read-promotion (spill_rp=1, spill_rp_clean=1; alias proposed_b12r) on the workloads where
# spill triggers: SPFresh trace + YCSB-A/B/C/E + TPC-C (aged, PREFILL_SEQ=1), same drivers as q_spill.sh -> results/final_rp_spillrp.
# (Ali-3 and raw fio never spill: identical to spill=2.) 2) then the remaining 5 baselines of the 128K-aligned QD32 replay
# (continuation of q_a128r.sh, whose page4k/proposed part is done).
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_rp_spillrp; mkdir -p $R
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
S="proposed:$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"
D=/mnt/nvme_mounted/dm_trace_sampled; TRACE=spfresh
idle; CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
echo "$(date) trace $TRACE memmap_size=$M"
TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$S"
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" POLICIES="proposed_b12r" \
  tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
A=/home/sejun000/dual_mapping/results/final_rp_a128
for cfg in "50 70" "50 30" "90 70" "90 30"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay $t : baselines"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$A/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "leaftl:cache_bytes=B12,leaftl_batch=128" "dftl:cache_bytes=B12,dftl_tpbuf=1" "iurmw:U=8" "fast:cache_bytes=B12" "mu:cache_bytes=B12"
done
echo "$(date) all done"
