#!/bin/bash
# (2026-11-13, user) DUET without spill/load (copy-or-merge only, proposedab_g32ns) on the workloads the ablation did not
# cover (50:50, 90:10, YCSB-C; 70:30, Alibaba, SPFresh, YCSB-A, YCSB-B, TPC-C already in results/final_32g/ablation),
# then Fig. 10 coarse-IU sensitivity with the same variant (U = 32 / 64 / 128; U = 16 = ablation ns runs).
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g; D=/mnt/nvme_mounted/dm_trace_32g
. tools/apps/common.sh >/dev/null 2>&1
sp() { for p in "$@"; do echo "${p%%_*}:$(policy_params $p)"; done; }
replay() { local t=$1 td=$2 o=$3; shift 3; idle; echo "$(date) $t -> $o : $*"
  TRACE=$t TRACE_DIR=$td RESULTS=$o tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp "$@"); }
apps() { local o=$1 w=$2; shift 2; idle; mkdir -p $o; echo "$(date) apps $w -> $o : $*"
  PREFILL_SEQ=1 RESULTS_ROOT=$o WORKLOADS="$w" POLICIES="$*" tools/apps/run_all.sh >> $o.run_all.log 2>&1; }
# --- main figures: the three missing workloads
replay a128_r50_s70 $D $R/ablation/ns/replay_a128_r50_s70 proposedab_g32ns
replay a128_r90_s70 $D $R/ablation/ns/replay_a128_r90_s70 proposedab_g32ns
apps $R/ablation/apps "ycsbc_d_t64" proposedab_g32ns
# --- Fig. 10 without spill
for u in 32 64 128; do
  replay a128_r70_s70 $D $R/usens_ns/u$u/replay_a128_r70_s70 proposedab_g32u${u}ns
  replay spfresh $D $R/usens_ns/u$u/trace_spfresh proposedab_g32u${u}ns
done
apps $R/usens_ns/apps "ycsba_d_t64 ycsbb_d_t64 tpcc_d_t64" proposedab_g32u32ns proposedab_g32u64ns proposedab_g32u128ns
echo "$(date) all done"
