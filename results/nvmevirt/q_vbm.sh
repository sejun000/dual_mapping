#!/bin/bash
# (2026-10-30) Valid-bitmap budget: every bounded FTL gets the total DRAM of IU+RMW (32 KiB IU) = its map (B12 = L x 4 / 8)
# + one valid bit per physical 32 KiB IU, and pays its own GC valid info and directory out of that total (each FTL then
# rounds down by its own allocation granularity):
#   DFTL / LeaFTL : one valid bit per physical page + their GTD (outside cache_bytes) -> cache_bytes = T - NP/8 - GTD
#   uFTL          : its own flash-resident invalid bitmap (32 KiB cache + 1 B/block)  -> cache_bytes = T - NP/8 - 32768 - NB
#                   (the per-page valid bit is charged as for DFTL as well)
#   DUET          : one bit per chunk + one bit per page of the 18 fine blocks        -> map_budget_ppm 125476
#   CoMap         : one bit per page of its NB - NLB - T = 23 non-data blocks          -> cache_bytes = T - 23 P/8
#   FAST          : none (data page LPN = block position, log pages in the log map); T buys the same 15 log blocks as
#                   B12 on every device -> identical runs, not rerun. page4k (unbounded) and IU+RMW (defines T) not rerun.
# Device geometry (page4k L, NB = 373, P): spfresh 5265216/15104, ali3 14546288/41728, apps 10798160/30976 (aliases *_vb in
# tools/apps/common.sh), replays 23559648/67584. DUET = proposed_b12q (fine quota 18) with the new map_budget_ppm.
# Workloads as q_q18.sh / q_comap.sh. -> results/final_vbm. Waits for q_kick.sh, then for an idle NVMeVirt before every step.
cd /home/sejun000/dual_mapping
while pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?results/nvmevirt/q_kick\.sh' >/dev/null; do sleep 60; done
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_vbm; mkdir -p $R
DU="U=16,fine_extra=0,fine_quota_ppm=48300,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125476,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"
specs() {   # $1 DFTL, $2 LeaFTL, $3 uFTL cache_bytes: the four FTLs on the normal namespace
  echo "proposed:$DU dftl:cache_bytes=$1,dftl_tpbuf=1 leaftl:cache_bytes=$2,leaftl_batch=128 mu:cache_bytes=$3"
}
D=/mnt/nvme_mounted/dm_trace_sampled
#          trace   DFTL    LeaFTL  uFTL    CoMap
for t in "spfresh 1995848 1934140 1983271 2677212" "ali3 5513952 5343484 5537631 7396372"; do set -- $t
  idle; CAP=$(python3 -c "import json;print(json.load(open('$D/$1.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $1 memmap_size=$M"
  TRACE=$1 RESULTS=$R/trace_$1 NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(specs $2 $3 $4) \
    "comap:cache_bytes=$5,comap_logfrac_ppm=900000"
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" \
  POLICIES="proposed_vb dftl_vb leaftl_vb mu_vb comap_vb" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
for cfg in "50 70" "70 70" "90 70" "50 30" "70 30" "90 30"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    $(specs 8930580 8654488 8989467)
  idle; echo "$(date) replay ${t}_fast : comap"
  TRACE=${t}_fast TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "comap:cache_bytes=11979408,comap_logfrac_ppm=900000"
done
echo "$(date) all done"
