#!/bin/bash
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
while busy; do sleep 60; done
echo "$(date) start"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_aged WORKLOADS="fio_mix70_d" \
  POLICIES="page4k proposed dftl leaftl fast mu iurmw" tools/apps/run_all.sh
echo "$(date) done"
