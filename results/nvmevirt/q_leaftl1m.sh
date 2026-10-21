#!/bin/bash
# LeaFTL re-run with a 1 MiB sort batch (leaftl_batch=256 = the common write-buffer size) on the 11 paper workloads.
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_rp
L="leaftl:cache_bytes=B12,leaftl_batch=256"
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" POLICIES="leaftl_b12" \
  tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
for cfg in "50 70" "50 30" "90 70" "90 30"; do set -- $cfg
  idle; echo "$(date) fio read=$1% 4K:128K=$2:$((100-$2))"
  READ=$1 BSSPLIT=4k/$2:128k/$((100-$2)) OUTROOT=$R/fio tools/nvmev_cycle.sh tools/nvmev_mixbs.sh _r$1_s$2 "$L"
done
D=/mnt/nvme_mounted/dm_trace_sampled
for TRACE in ali3 spfresh; do
  idle
  CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])")
  M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $TRACE memmap_size=$M"
  TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$L"
done
echo "$(date) all done"
