#!/bin/bash
# (2026-11-13, user) reordered ablation (Fig. 8): Basic + cuckoo (bk) -> spill/load instead of cuckoo (bs)
# -> + coarse-or-fine (bsc) -> + copy-or-merge = DUET (proposed_g32 main runs, reused). Basic = ablation/nsfm (reused).
# Workloads: YCSB-A, YCSB-B, Alibaba, 70:30. -> results/final_32g/ablation2
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g/ablation2; D=/mnt/nvme_mounted/dm_trace_32g; mkdir -p $R
. tools/apps/common.sh >/dev/null 2>&1
sp() { for p in "$@"; do echo "${p%%_*}:$(policy_params $p)"; done; }
replay() { local t=$1 td=$2 o=$3; shift 3; idle; echo "$(date) $t -> $o : $*"
  TRACE=$t TRACE_DIR=$td RESULTS=$o tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp "$@"); }
for v in bk bs bsc; do
  replay a128_r70_s70 $D $R/$v/replay_a128_r70_s70 proposedab_g32$v
  replay ali3 $D $R/$v/trace_ali3 proposedab_g32$v
done
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64" \
  POLICIES="proposedab_g32bk proposedab_g32bs proposedab_g32bsc" tools/apps/run_all.sh >> $R/apps.run_all.log 2>&1
echo "$(date) all done"
