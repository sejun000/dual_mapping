#!/bin/bash
# Master queue 2 (after the current splitEval YCSB-B/C matrix):
# 1) reduced client concurrency (splitEval used 320 threads/terminals in total): 64 total x 7 policies, 16 total x 4
# 2) IU+RMW on the splitEval-setting workloads, 3) raw-device fio conditions incl. IU+RMW, IU+RMW full random write
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
P="proposed:U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500"
export RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_ext4
idle; echo "$(date) 1) reduced concurrency"
WORKLOADS="ycsbb_d_t64 ycsba_d_t64 tpcc_d_t64 ycsbf_d_t64" POLICIES="page4k proposed dftl leaftl fast mu iurmw" tools/apps/run_all.sh >> $RESULTS_ROOT/run_all_lowqd.log 2>&1
idle; WORKLOADS="ycsbb_d_t16 ycsba_d_t16" POLICIES="page4k proposed dftl leaftl" tools/apps/run_all.sh >> $RESULTS_ROOT/run_all_lowqd.log 2>&1
idle; echo "$(date) 2) splitEval YCSB-B/C leftovers + IU+RMW"
WORKLOADS="ycsbb ycsbc" POLICIES="page4k proposed dftl leaftl fast mu iurmw" tools/apps/run_all.sh >> $RESULTS_ROOT/run_all_read.log 2>&1
idle; POLICIES=iurmw tools/apps/run_all.sh >> $RESULTS_ROOT/run_all_iurmw.log 2>&1
unset RESULTS_ROOT
idle; echo "$(date) 3) raw fio"
for cfg in "0.9 95 _mix95_z09" "1.2 50 _mix50_z12" "0.9 70 _mix70_z09"; do
  set -- $cfg; idle
  THETA=$1 READ=$2 tools/nvmev_cycle.sh tools/nvmev_mixed.sh $3 page4k "$P" "dftl:cache_bytes=B18,dftl_tpbuf=1" "leaftl:cache_bytes=B18" fast "mu:cache_bytes=B18" iurmw
done
for cfg in "0.9 50 _mix50_z09" "0.99 50 _mix50_z099"; do
  set -- $cfg; idle; THETA=$1 READ=$2 tools/nvmev_cycle.sh tools/nvmev_mixed.sh $3 iurmw
done
idle; tools/nvmev_cycle.sh tools/nvmev_randwrite.sh _op7 iurmw
echo "$(date) all done"
