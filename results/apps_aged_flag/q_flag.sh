#!/bin/bash
# aged-device app condition, proposed only, rebuilt ~/nvmevirt_proposed (flag-bit coarse entry, no per-IU bitmap)
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
if busy; then echo "$(date) device busy, not starting"; exit 1; fi
echo "$(date) flag-bit aged runs start"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_aged_flag \
  WORKLOADS="ycsbb_d_t64 fio_mix70_d" POLICIES="proposed" tools/apps/run_all.sh
echo "$(date) flag-bit aged runs done"
