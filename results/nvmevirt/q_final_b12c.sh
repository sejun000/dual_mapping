#!/bin/bash
# after q_final_b12b: real traces Alibaba 1/3/2 + SSD at 12.5%; page4k is budget-independent -> reused from results/nvmevirt/trace_<t>/page4k. Then a Varmail recheck.
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
while pgrep -f '^/bin/bash results/nvmevirt/q_final_b12b.sh' >/dev/null; do sleep 60; done
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
P12F="$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4"
POL=("proposed:$P12F" "dftl:cache_bytes=B12,dftl_tpbuf=1" "leaftl:cache_bytes=B12" "fast:cache_bytes=B12" "mu:cache_bytes=B12" "iurmw:U=8")
R=/home/sejun000/dual_mapping/results/final_b12
D=/mnt/nvme_mounted/dm_trace_sampled
for TRACE in ali1 ali3 ali2 ssd; do
  idle
  CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])") || { echo "no $TRACE.json"; continue; }
  M=$(python3 tools/nvmev_geom.py --fit $CAP) || { echo "$TRACE does not fit"; continue; }
  echo "$(date) trace $TRACE: capacity $CAP B, memmap_size=$M"
  TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "${POL[@]}"
done
idle; echo "$(date) Varmail recheck"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps_recheck WORKLOADS="vmhot1g" POLICIES="page4k proposed_b12f" tools/apps/run_all.sh
echo "$(date) all done"
