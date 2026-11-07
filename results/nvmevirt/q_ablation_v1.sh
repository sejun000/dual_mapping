#!/bin/bash
# (2026-11-06) Ablation of DUET, cumulative removals from the final DUET (q_vbm2.sh), on ~/nvmevirt_proposedab
# (= ~/nvmevirt_proposedst + fold_all switch; fine-area counters on every [dm ...] line):
#   ns   : - spill (a full bucket pair merges its victim coarse IU instead of spilling its pointers)
#   nsf  : ns - GC choice by valid count (gc_select_cost=0: coarse GC unless the fine area is at its cap)
#   nsfm : nsf - merge cost model (fold_all=1: fine GC merges every coarse IU of the victim)
# Three workloads: YCSB-A (app), Ali3 (real trace), R50 70:30 (raw I/O). Full DUET = results/final_vbm and final_stats.
# Starts after q_stats.sh. -> results/final_ablation/<variant>/...
cd /home/sejun000/dual_mapping
while pgrep -f "^/bin/bash (/home/sejun000/dual_mapping/)?results/nvmevirt/q_stats\.sh" >/dev/null; do sleep 60; done
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_ablation; mkdir -p $R
. tools/apps/common.sh >/dev/null 2>&1
par() { policy_params proposedab_$1; }
idle; echo "$(date) apps YCSB-A"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64" \
  POLICIES="proposedab_ns proposedab_nsf proposedab_nsfm" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
D=/mnt/nvme_mounted/dm_trace_sampled
CAP=$(python3 -c "import json;print(json.load(open('$D/ali3.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
for v in ns nsf nsfm; do
  idle; echo "$(date) trace ali3 $v memmap_size=$M params=$(par $v)"
  TRACE=ali3 RESULTS=$R/$v/trace_ali3 NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposedab:$(par $v)"
done
t=a128_r50_s70
for v in ns nsf nsfm; do
  idle; echo "$(date) replay $t $v params=$(par $v)"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/$v/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    "proposedab:$(par $v)"
done
echo "$(date) all done"
