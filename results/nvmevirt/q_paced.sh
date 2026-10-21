#!/bin/bash
# Paper subset re-run with paced background NAND ops (dm_ftl.c DM_BG_*), 12.5% mapping DRAM, 7 policies.
# 11 workloads: raw fio R50/R90 x 4K-heavy/128K-heavy; apps YCSB-A/B/C/E + TPC-C (aged device); traces Ali-3, SPFresh.
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
while pgrep -f 'pace_test.sh' >/dev/null; do sleep 60; done
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
P12F="$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4"
POL=(page4k "proposed:$P12F" "dftl:cache_bytes=B12,dftl_tpbuf=1" "leaftl:cache_bytes=B12" "fast:cache_bytes=B12" "mu:cache_bytes=B12" "iurmw:U=8")
R=/home/sejun000/dual_mapping/results/final_b12p
for cfg in "50 70" "50 30" "90 70" "90 30"; do set -- $cfg
  idle; echo "$(date) fio read=$1% 4K:128K=$2:$((100-$2))"
  READ=$1 BSSPLIT=4k/$2:128k/$((100-$2)) OUTROOT=$R/fio tools/nvmev_cycle.sh tools/nvmev_mixbs.sh _r$1_s$2 "${POL[@]}"
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" \
  POLICIES="page4k proposed_b12f dftl_b12 leaftl_b12 fast mu_b12 iurmw" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
D=/mnt/nvme_mounted/dm_trace_sampled
for TRACE in ali3 spfresh; do
  idle
  CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])")
  M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $TRACE memmap_size=$M"
  TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "${POL[@]}"
done
echo "$(date) all done"
