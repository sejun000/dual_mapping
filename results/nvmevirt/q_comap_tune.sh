#!/bin/bash
# (2026-11-08) CoMap with tuned parameters (comap_logfrac_ppm 990000, comap_p0 990000; ftlsim sweep results/comap_sim/tune)
# on the 10 paper workloads, same budgets as q_vbm.sh's comap (traces / raw) and comap_vb (apps). -> results/final_comap_tune
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_comap_tune; mkdir -p $R
X="comap_logfrac_ppm=990000,comap_p0=990000"
for cfg in "50 70" "70 70" "90 70"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay ${t}_fast : comap"
  TRACE=${t}_fast TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "comap:cache_bytes=11979408,$X"
done
D=/mnt/nvme_mounted/dm_trace_sampled
for t in "ali3 7396372" "spfresh 2677212"; do set -- $t
  idle; CAP=$(python3 -c "import json;print(json.load(open('$D/$1.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $1 memmap_size=$M"
  TRACE=$1 RESULTS=$R/trace_$1 NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "comap:cache_bytes=$2,$X"
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" \
  POLICIES="comap_vbt" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
echo "$(date) all done"
