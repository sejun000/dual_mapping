#!/bin/bash
# Pilot with size-aligned 4K/128K mix: raw fio 70:30, 4K:128K 50:50 at 12.5% and 18.75% (+page4k); then YCSB-A 12.5% proposed+fix
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
P12F="$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4"; P18F="$V,map_budget_ppm=187500,onefront_fix=1,pool_cap=4"
idle; echo "$(date) fio 70:30 4K:128K 50:50 aligned @12.5%"
READ=70 MIX=50:50 tools/nvmev_cycle.sh tools/nvmev_mixbs2.sh _r70_m50_b12 "proposed:$P12F" "dftl:cache_bytes=B12,dftl_tpbuf=1" "leaftl:cache_bytes=B12"
idle; echo "$(date) fio 70:30 4K:128K 50:50 aligned @18.75% + page4k"
READ=70 MIX=50:50 tools/nvmev_cycle.sh tools/nvmev_mixbs2.sh _r70_m50_b18 page4k "proposed:$P18F" "dftl:cache_bytes=B18,dftl_tpbuf=1" "leaftl:cache_bytes=B18"
idle; echo "$(date) YCSB-A 12.5% proposed+fix"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_b12 WORKLOADS="ycsba_d_t64" POLICIES="proposed_b12f" tools/apps/run_all.sh
echo "$(date) pilot done"
