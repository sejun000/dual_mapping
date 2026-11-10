#!/bin/bash
# (2026-11-07) LeaFTL rerun with misprediction (OOB) reads printed: the [dm ...] stats line now ends with
# read_other=N (DM_R_OTHER = LeaFTL misprediction / boundary-page reads, issued and timed before but not printed).
# Same configs as q_vbm.sh (leaftl only, 10 paper workloads). -> results/final_leaftl_mp
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_leaftl_mp; mkdir -p $R
for cfg in "50 70" "70 70" "90 70"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "leaftl:cache_bytes=8654488,leaftl_batch=128"
done
D=/mnt/nvme_mounted/dm_trace_sampled
for t in "ali3 5343484" "spfresh 1934140"; do set -- $t
  idle; CAP=$(python3 -c "import json;print(json.load(open('$D/$1.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $1 memmap_size=$M"
  TRACE=$1 RESULTS=$R/trace_$1 NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "leaftl:cache_bytes=$2,leaftl_batch=128"
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" \
  POLICIES="leaftl_vb" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
echo "$(date) all done"
