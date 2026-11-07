#!/bin/bash
# (2026-10-29) CoMap baseline (Zhou et al., DATE'26; ~/nvmevirt_comap = ftlsim mode=comap, see dm/NOTES_comap.md) on the paper
# workload set with the drivers of q_q18.sh: SPFresh + Ali-3 traces, YCSB-A/B/C/E + TPC-C (aged, PREFILL_SEQ=1), raw
# 128K-aligned QD32 replays R50/R70/R90 x 7:3 / 3:7. Mapping DRAM = B12 (cache_bytes = its whole budget: block map + bitmap +
# GTD + FAST log map + DFTL CMT). CoMap's block side is FAST's, so it exposes whole logical blocks only (L - L % P, the same
# namespace as FAST): the raw replays use the *_fast traces (same generator/seed at FAST's capacity), as FAST did
# (q_fast_a128.sh / q_r70.sh); nvmev_geom.py --fit already sizes the SPFresh / Ali-3 devices for FAST's namespace.
# -> results/final_comap. Waits for q_q18.sh to finish, then for an idle NVMeVirt before every step.
cd /home/sejun000/dual_mapping
while pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?results/nvmevirt/q_q18\.sh' >/dev/null; do sleep 60; done
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
[ -f ~/nvmevirt_comap/nvmev.ko ] || { echo "missing ~/nvmevirt_comap/nvmev.ko"; exit 1; }
R=/home/sejun000/dual_mapping/results/final_comap; mkdir -p $R
S="comap:cache_bytes=B12,comap_logfrac_ppm=900000"
D=/mnt/nvme_mounted/dm_trace_sampled
for TRACE in spfresh ali3; do
  idle; CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $TRACE memmap_size=$M"
  TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$S"
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" POLICIES="comap" \
  tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
for cfg in "50 70" "70 70" "90 70" "50 30" "70 30" "90 30"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay ${t}_fast : comap"
  TRACE=${t}_fast TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$S"
done
echo "$(date) all done"
