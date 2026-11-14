#!/bin/bash
# (2026-11-13, user) rerun of Fig. 10 copy-or-merge U=32 SPFresh: the first run was cut at 96.5% by the 02:08 UTC reboot
# (kept as usens_ns/u32/trace_spfresh/proposedab.failed_reboot0208).
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_32g; D=/mnt/nvme_mounted/dm_trace_32g
. tools/apps/common.sh >/dev/null 2>&1
idle; echo "$(date) spfresh -> $R/usens_ns/u32/trace_spfresh : proposedab_g32u32ns"
TRACE=spfresh TRACE_DIR=$D RESULTS=$R/usens_ns/u32/trace_spfresh tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposedab:$(policy_params proposedab_g32u32ns)"
echo "$(date) all done"
