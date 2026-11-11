#!/bin/bash
# (2026-11-11) Full rerun on ONE device for every workload: 32 GB host-visible (L = 8391056 pages; FAST/CoMap whole-block
# L = 8388608 = 32 GiB), OP 7%, 274 superblocks of 128 MiB (8 MiB NAND block per die x 16 dies). All ~/nvmevirt_* trees
# rebuilt with BLKS_PER_PLN 274 (old modules kept as nvmev.ko.nb373). memmap_size 35073M = 274 x 16 x 8 MiB + 1 MiB.
# Budgets: tools/budget_vb.py 35073M 274 (aliases *_g32 in tools/apps/common.sh). Traces: /mnt/nvme_mounted/dm_trace_32g
# (results/nvmevirt/gen_32g.sh; SPFresh regenerated at region=4096 rtouch=1 order=addr, 26.9 GiB, 2026-11-11). Seven FTLs: Page, DUET, DFTL, LeaFTL, FAST, CoMap (tuned), CoarseIU (IU+RMW, U=8).
# Then ablation (Fig. 8), time series + fine stats (Fig. 9, proposedst), coarse-IU sensitivity (Fig. 10). -> results/final_32g
# (2026-11-11 07:2x) q_32g_rest.sh = the rest of q_32g.sh after its apps step (q_32g.sh stopped while its apps run_all
# kept running): the ablation and the Fig. 9 time series run on both the 50:50 raw I/O and SPFresh (user), and DUET with
# cuckoo displacement before a spill (proposed_g32k, ct_kick=500) runs on the ablation workloads.
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g; mkdir -p $R
D=/mnt/nvme_mounted/dm_trace_32g
. tools/apps/common.sh >/dev/null 2>&1
sp() { for p in "$@"; do echo "${p%%_*}:$(policy_params $p)"; done; }       # alias -> folder:params for nvmev_cycle
ALL="page4k_g32 proposed_g32 dftl_g32 leaftl_g32 fast_g32 comap_g32 iurmw_g32"
APPS="page4k_g32 proposed_g32 dftl_g32 leaftl_g32 fast_g32 comap_g32 iurmw_g32"
poll() { while true; do
    if [ -e /sys/module/nvmev/parameters/dm_dump ]; then
      echo 1 | sudo tee /sys/module/nvmev/parameters/dm_dump >/dev/null 2>&1
      l=$(sudo dmesg | grep '\[dm flush\].*smp_n=' | tail -1); [ -n "$l" ] && echo "$(date +%s) $l" >> "$1"
    fi; sleep 5; done; }
run_ts() { local o=$1; shift; mkdir -p $o; poll $o/ts.log & local pp=$!; "$@"; kill $pp; wait $pp 2>/dev/null; }
replay() {   # $1 trace name, $2 trace dir, $3 results dir, rest = aliases
  local t=$1 td=$2 o=$3; shift 3; idle; echo "$(date) $t -> $o : $*"
  TRACE=$t TRACE_DIR=$td RESULTS=$o tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp "$@")
}
apps() {     # $1 results root, $2 workloads, rest = aliases
  local o=$1 w=$2; shift 2; idle; echo "$(date) apps $w -> $o : $*"
  PREFILL_SEQ=1 RESULTS_ROOT=$o WORKLOADS="$w" POLICIES="$*" tools/apps/run_all.sh >> $o.run_all.log 2>&1
}
wait_traces() {   # SPFresh: unsampled, never-written pages squeezed out (region=4096, address order) -> 22.8 GiB footprint
  while pgrep -f '^/home/sejun000/dual_mapping/tools/trace_prep ' >/dev/null; do sleep 60; done
  python3 -c "import json;assert json.load(open('$D/spfresh.json'))['capacity_bytes'] <= 34359738368" || rm -f $D/spfresh.csv
}
# --- continue after the apps step of q_32g.sh (its run_all.sh is still running; idle() waits for it)
wait_traces
replay ali3 $D $R/trace_ali3 $ALL
[ -e $D/spfresh.csv ] && replay spfresh $D $R/trace_spfresh $ALL
replay a128_r70_s70 $D $R/replay_a128_r70_s70 $ALL
replay a128_r90_s70 $D $R/replay_a128_r90_s70 $ALL
# --- Fig. 9 time series (+ fine stats for Fig. 8): instrumented DUET, decisions unchanged
idle; echo "$(date) ts"
TRACE=a128_r50_s70 TRACE_DIR=$D RESULTS=$R/ts/replay_a128_r50_s70 run_ts $R/ts/replay_a128_r50_s70 tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp proposedst_g32)
idle; TRACE=ali3 TRACE_DIR=$D RESULTS=$R/ts/trace_ali3 run_ts $R/ts/trace_ali3 tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp proposedst_g32)
idle; TRACE=spfresh TRACE_DIR=$D RESULTS=$R/ts/trace_spfresh run_ts $R/ts/trace_spfresh tools/nvmev_cycle.sh tools/nvmev_trace.sh "" $(sp proposedst_g32)
idle; PREFILL_SEQ=1 RESULTS_ROOT=$R/ts/apps WORKLOADS=ycsba_d_t64 POLICIES=proposedst_g32 run_ts $R/ts/apps/ycsba_d_t64 tools/apps/run_all.sh >> $R/ts.run_all.log 2>&1
# --- Fig. 8 ablation: YCSB-A, Alibaba, 50:50 raw I/O, SPFresh
for v in ns nsm nsfm; do
  replay a128_r50_s70 $D $R/ablation/$v/replay_a128_r50_s70 proposedab_g32$v
  replay spfresh $D $R/ablation/$v/trace_spfresh proposedab_g32$v
  replay ali3 $D $R/ablation/$v/trace_ali3 proposedab_g32$v
done
apps $R/ablation/apps ycsba_d_t64 proposedab_g32ns proposedab_g32nsm proposedab_g32nsfm
# --- DUET + cuckoo displacement before a spill (ct_kick=500), same workloads as the ablation
replay a128_r50_s70 $D $R/kick/replay_a128_r50_s70 proposed_g32k
replay ali3 $D $R/kick/trace_ali3 proposed_g32k
replay spfresh $D $R/kick/trace_spfresh proposed_g32k
apps $R/kick/apps ycsba_d_t64 proposed_g32k
# --- Fig. 10 coarse-IU sensitivity (U = 16 is the main matrix)
for u in 32 64 128; do
  replay a128_r50_s70 $D $R/usens/u$u/replay_a128_r50_s70 proposed_g32u$u
  replay ali3 $D $R/usens/u$u/trace_ali3 proposed_g32u$u
  [ -e $D/spfresh.csv ] && replay spfresh $D $R/usens/u$u/trace_spfresh proposed_g32u$u
done
apps $R/usens/apps "ycsba_d_t64 tpcc_d_t64" proposed_g32u32 proposed_g32u64 proposed_g32u128
echo "$(date) all done"
