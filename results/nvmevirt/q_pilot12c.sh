#!/bin/bash
# after q_pilot12b: redo the YCSB-A 12.5% proposed+fix run that was stopped for the admin.c patch
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
while pgrep -f '^/bin/bash results/nvmevirt/q_pilot12b.sh' >/dev/null; do sleep 60; done
while busy; do sleep 60; done
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_b12 WORKLOADS="ycsba_d_t64" POLICIES="proposed_b12f" tools/apps/run_all.sh
echo "$(date) done"
