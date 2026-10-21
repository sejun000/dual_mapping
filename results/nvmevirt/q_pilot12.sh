#!/bin/bash
# Pilot for the DRAM-budget decision (12.5% vs 18.75%): YCSB-A aged at 12.5%, raw fio 70:30 4K/128K 50:50 at both budgets
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
P18="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500"
P12="${P18/map_budget_ppm=187500/map_budget_ppm=125000}"
idle; echo "$(date) pilot: YCSB-A 12.5%"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_b12 WORKLOADS="ycsba_d_t64" \
  POLICIES="proposed_b12 dftl_b12 leaftl_b12" tools/apps/run_all.sh
idle; echo "$(date) pilot: raw fio 70:30, 4K/128K 50:50, 12.5%"
READ=70 BSSPLIT=4k/50:128k/50 tools/nvmev_cycle.sh tools/nvmev_mixbs.sh _r70_s50_b12 "proposed:$P12" "dftl:cache_bytes=B12,dftl_tpbuf=1" "leaftl:cache_bytes=B12"
idle; echo "$(date) pilot: raw fio 70:30, 4K/128K 50:50, 18.75% + page4k"
READ=70 BSSPLIT=4k/50:128k/50 tools/nvmev_cycle.sh tools/nvmev_mixbs.sh _r70_s50_b18 page4k "proposed:$P18" "dftl:cache_bytes=B18,dftl_tpbuf=1" "leaftl:cache_bytes=B18"
echo "$(date) pilot done"
