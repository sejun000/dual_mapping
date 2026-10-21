#!/bin/bash
# main app condition: aged device (whole-device sequential prefill before mkfs), 7 core workloads x 7 policies
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
while busy; do sleep 60; done
echo "$(date) aged-device matrix start"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_aged \
  WORKLOADS="ycsbb_d_t64 ycsba_d_t64 ycsbfstd_d_t64 tpcc_d_t64 fio_randwrite_d fio_mix_d fileserverfs" \
  POLICIES="page4k proposed dftl leaftl fast mu iurmw" tools/apps/run_all.sh
echo "$(date) aged-device matrix done"
