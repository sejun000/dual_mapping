#!/bin/bash
# after the running raw-fio 70:30 cycle: standard YCSB-F (64 and 320 threads) x 7, then the remaining IU+RMW raw runs
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
idle; echo "$(date) standard YCSB-F"
RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_ext4 WORKLOADS="ycsbfstd_d_t64 ycsbfstd_d" POLICIES="page4k proposed dftl leaftl fast mu iurmw" \
  tools/apps/run_all.sh >> results/apps_ext4/run_all_fstd.log 2>&1
for cfg in "0.9 50 _mix50_z09" "0.99 50 _mix50_z099"; do
  set -- $cfg; idle; THETA=$1 READ=$2 tools/nvmev_cycle.sh tools/nvmev_mixed.sh $3 iurmw
done
idle; tools/nvmev_cycle.sh tools/nvmev_randwrite.sh _op7 iurmw
echo "$(date) all done"
