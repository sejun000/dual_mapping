#!/bin/bash
# (2026-10-30) Replaces the rest of q_vbm.sh + q_du4.sh (both stopped; the running SPFresh cycle of q_vbm.sh finishes).
# DUET = four active blocks (coarse_class=0, uni_onefront=0) with the valid-bitmap budget (map_budget_ppm 125476).
# The old-DUET SPFresh result is kept as trace_spfresh/proposed_cc2. Same workloads / budgets as q_vbm.sh.
cd /home/sejun000/dual_mapping
while kill -0 2782305 2>/dev/null; do sleep 60; done                # q_vbm.sh's SPFresh cycle
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_vbm
DU="U=16,fine_extra=0,fine_quota_ppm=48300,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=0,adapt=2,uni_onefront=0,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125476,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"
D=/mnt/nvme_mounted/dm_trace_sampled
geom() { CAP=$(python3 -c "import json;print(json.load(open('$D/$1.json'))['capacity_bytes'])"); python3 tools/nvmev_geom.py --fit $CAP; }
idle
[ -d $R/trace_spfresh/proposed ] && mv $R/trace_spfresh/proposed $R/trace_spfresh/proposed_cc2
M=$(geom spfresh); echo "$(date) trace spfresh (DUET only) memmap_size=$M"
TRACE=spfresh RESULTS=$R/trace_spfresh NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposed:$DU"
idle; M=$(geom ali3); echo "$(date) trace ali3 memmap_size=$M"
TRACE=ali3 RESULTS=$R/trace_ali3 NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposed:$DU" \
  dftl:cache_bytes=5513952,dftl_tpbuf=1 leaftl:cache_bytes=5343484,leaftl_batch=128 mu:cache_bytes=5537631 \
  comap:cache_bytes=7396372,comap_logfrac_ppm=900000
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" \
  POLICIES="proposed_vb dftl_vb leaftl_vb mu_vb comap_vb" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
for cfg in "50 70" "70 70" "90 70" "50 30" "70 30" "90 30"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "proposed:$DU" dftl:cache_bytes=8930580,dftl_tpbuf=1 leaftl:cache_bytes=8654488,leaftl_batch=128 mu:cache_bytes=8989467
  idle; echo "$(date) replay ${t}_fast : comap"
  TRACE=${t}_fast TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "comap:cache_bytes=11979408,comap_logfrac_ppm=900000"
done
echo "$(date) all done"
