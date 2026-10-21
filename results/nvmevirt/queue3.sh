#!/bin/bash
# after the main ext4 suite: IU+RMW app runs -> remaining fio conditions (now incl. iurmw) -> iurmw for finished fio conditions
cd /home/sejun000/dual_mapping
P="proposed:U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500"
while pgrep -f "[r]un_all.sh" >/dev/null || lsmod | grep -q '^nvmev '; do sleep 60; done
echo "$(date) main ext4 suite done; IU+RMW apps"
export RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_ext4
POLICIES=iurmw tools/apps/run_all.sh 2>&1 | tee -a $RESULTS_ROOT/run_all_iurmw.log
unset RESULTS_ROOT
while pgrep -f "[r]un_all.sh" >/dev/null || lsmod | grep -q '^nvmev '; do sleep 60; done
echo "$(date) remaining fio conditions"
for cfg in "1.2 50 _mix50_z12" "0.9 70 _mix70_z09"; do
  set -- $cfg
  THETA=$1 READ=$2 tools/nvmev_cycle.sh tools/nvmev_mixed.sh $3 page4k "$P" "dftl:cache_bytes=B18,dftl_tpbuf=1" "leaftl:cache_bytes=B18" fast "mu:cache_bytes=B18" iurmw
done
for cfg in "0.9 50 _mix50_z09" "0.99 50 _mix50_z099"; do
  set -- $cfg
  THETA=$1 READ=$2 tools/nvmev_cycle.sh tools/nvmev_mixed.sh $3 iurmw
done
echo "$(date) all done"
