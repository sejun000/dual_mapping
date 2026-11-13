#!/bin/bash
# (2026-11-13) rest of q_32g_ns.sh after the 02:10 UTC reboot into kernel 6.8.0-149 (modules rebuilt): Fig. 10 without
# spill at U = 64 / 128 and the Fig. 10 apps. Done before the reboot: ns 50:50, 90:10, YCSB-C; usens_ns U = 32 (70:30, SPFresh).
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g; D=/mnt/nvme_mounted/dm_trace_32g
. tools/apps/common.sh >/dev/null 2>&1
sp() { for p in "$@"; do echo "${p%%_*}:$(policy_params $p)"; done; }
replay() { local t=$1 td=$2 o=$3; shift 3; idle; echo "$(date) $t -> $o : $*"
  TRACE=$t TRACE_DIR=$td RESULTS=$o tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp "$@"); }
for u in 64 128; do
  replay a128_r70_s70 $D $R/usens_ns/u$u/replay_a128_r70_s70 proposed_g32u${u}ns
  replay spfresh $D $R/usens_ns/u$u/trace_spfresh proposed_g32u${u}ns
done
idle; mkdir -p $R/usens_ns/apps; echo "$(date) apps usens_ns"
PREFILL_SEQ=1 RESULTS_ROOT=$R/usens_ns/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 tpcc_d_t64" \
  POLICIES="proposedab_g32u32ns proposed_g32u64ns proposed_g32u128ns" tools/apps/run_all.sh >> $R/usens_ns/apps.run_all.log 2>&1
echo "$(date) all done"
