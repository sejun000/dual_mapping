#!/bin/bash
# (2026-11-13, user, urgent) ablation v3: Basic = Basic + cuckoo (bk) -> + coarse-or-fine (bkc) -> + copy-or-merge
# (= nskl, existing) -> + spill/load instead of cuckoo (= DUET, existing). New: bkc on 5 workloads, bk on SPFresh.
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g/ablation2; D=/mnt/nvme_mounted/dm_trace_32g
. tools/apps/common.sh >/dev/null 2>&1
sp() { for p in "$@"; do echo "${p%%_*}:$(policy_params $p)"; done; }
replay() { local t=$1 td=$2 o=$3; shift 3; [ -s $o/proposedab/result.json ] && { echo "$(date) skip $o"; return; }; idle; echo "$(date) $t -> $o : $*"
  TRACE=$t TRACE_DIR=$td RESULTS=$o tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp "$@"); }
idle; echo "$(date) apps bkc"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64" POLICIES="proposedab_g32bkc" \
  tools/apps/run_all.sh >> $R/apps.run_all.log 2>&1
replay ali3 $D $R/bkc/trace_ali3 proposedab_g32bkc
replay a128_r70_s70 $D $R/bkc/replay_a128_r70_s70 proposedab_g32bkc
replay spfresh $D $R/bk/trace_spfresh proposedab_g32bk
replay spfresh $D $R/bkc/trace_spfresh proposedab_g32bkc
echo "$(date) all done"
