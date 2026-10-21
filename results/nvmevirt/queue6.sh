#!/bin/bash
# after queue5: PostgreSQL-16-stock-settings rerun of the PG workloads (x7 policies), then the trace-replay queue
cd /home/sejun000/dual_mapping
idle() { while pgrep -f "[r]un_all.sh" >/dev/null || pgrep -f "[n]vmev_cycle.sh" >/dev/null || lsmod | grep -q '^nvmev '; do sleep 60; done; }
while pgrep -f "[q]ueue5.sh" >/dev/null; do sleep 60; done
idle; echo "$(date) queue5 done; PG stock-settings apps"
RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_ext4 WORKLOADS="ycsba_d ycsbf_d tpcc_d ycsbb_d ycsbc_d" \
  POLICIES="page4k proposed dftl leaftl fast mu iurmw" tools/apps/run_all.sh 2>&1 | tee -a results/apps_ext4/run_all_pgdefault.log
idle; echo "$(date) PG stock-settings done; trace replay"
exec results/nvmevirt/queue_trace.sh >> results/nvmevirt/queue_trace.log 2>&1
