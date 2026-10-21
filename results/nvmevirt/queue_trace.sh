#!/bin/bash
# Real-trace replay on NVMeVirt (closed loop QD32), after queue5.sh. Waits until queue5.sh has exited and the device is
# idle (same idle() as queue5.sh), then for each trace loads every policy's module with a memmap_size whose logical space
# holds the sampled trace (tools/nvmev_geom.py --fit) and runs tools/nvmev_trace.sh.
# Traces: /mnt/nvme_mounted/dm_trace_sampled/<t>.csv (tools/trace_prep reads=1 order=touch sample=k/1000; see the .json).
# Results: results/nvmevirt/trace_<t>/<policy>/ and results/nvmevirt/trace_<t>/SUMMARY.txt. Log: queue_trace.log.
cd /home/sejun000/dual_mapping
P="proposed:U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500"
idle() { while pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; do sleep 60; done; }
D=/mnt/nvme_mounted/dm_trace_sampled
echo "$(date) queue_trace: waiting for queue5.sh"
[ -n "$SKIP_Q5_WAIT" ] || while pgrep -f "^/bin/bash results/nvmevirt/queue5.sh" >/dev/null; do sleep 60; done
idle; echo "$(date) queue5 done, device idle"
for TRACE in ${TRACES:-spfresh kv_w1 ali1 ali3 ali2 ssd}; do
  idle
  CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])") || { echo "no $TRACE.json"; continue; }
  M=$(python3 tools/nvmev_geom.py --fit $CAP) || { echo "$TRACE does not fit"; continue; }
  echo "$(date) trace $TRACE: capacity $CAP B, memmap_size=$M"
  TRACE=$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" page4k "$P" "dftl:cache_bytes=B18,dftl_tpbuf=1" "leaftl:cache_bytes=B18" fast "mu:cache_bytes=B18" iurmw
  echo "$(date) trace $TRACE done"; cat results/nvmevirt/trace_$TRACE/SUMMARY.txt
done
echo "$(date) queue_trace all done"
