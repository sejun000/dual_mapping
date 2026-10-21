#!/bin/bash
# ext4 application suite: 8 workloads x 6 policies (tools/apps/run_all.sh, resumable from results/apps_ext4/*/*/STATUS).
# Modules are loaded with dm_flush_dump=0; device counters are taken via /sys/module/nvmev/parameters/dm_dump.
# Started (foreground) by results/nvmevirt/queue2.sh once results/apps_ext4/READY exists; safe to re-run after an
# interruption (run `tools/apps/run_all.sh --cleanup` first if a run was killed mid-way).
cd /home/sejun000/dual_mapping || exit 1
export RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_ext4
tools/apps/run_all.sh 2>&1 | tee -a "$RESULTS_ROOT/run_all.log"
exit "${PIPESTATUS[0]}"
