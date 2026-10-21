#!/bin/bash
# queued after the splitEval app runs: extra fio mixed-workload conditions x 6 policies
cd /home/sejun000/dual_mapping
sleep 300
while pgrep -f "[r]un_all.sh" >/dev/null || pgrep -f "[n]vmev_cycle.sh" >/dev/null || lsmod | grep -q '^nvmev '; do sleep 120; done
for cfg in "0.99 50 _mix50_z099" "1.2 50 _mix50_z12" "0.9 70 _mix70_z09"; do
  set -- $cfg
  THETA=$1 READ=$2 tools/nvmev_cycle.sh tools/nvmev_mixed.sh $3 page4k "proposed:U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500" "dftl:cache_bytes=B18,dftl_tpbuf=1" "leaftl:cache_bytes=B18" fast "mu:cache_bytes=B18"
done
