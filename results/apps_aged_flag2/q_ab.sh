#!/bin/bash
# NVMeVirt A/B on the aged-device condition, machine idle of simulations:
#  1) flag-bit core (optimized scans) on ycsbb_d_t64 + fio_mix70_d  -> results/apps_aged_flag2
#  2) the pre-change module (per-IU bitmap) on fio_mix70_d           -> results/apps_aged_flag_ab_old
SP=/tmp/claude-1001/-home-sejun000-dual-mapping/64054dd3-9fa1-4be8-986a-b7744c0b8ecf/scratchpad
KO=/home/sejun000/nvmevirt_proposed/nvmev.ko
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
if busy; then echo "$(date) device busy, not starting"; exit 1; fi
cmp -s $KO $SP/nvmev_backup/nvmev.ko.flag_opt || { echo "unexpected ko"; exit 1; }
echo "$(date) A: flag-bit (optimized) start"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_aged_flag2 \
  WORKLOADS="ycsbb_d_t64 fio_mix70_d" POLICIES="proposed" tools/apps/run_all.sh
if busy; then echo "$(date) device still busy after A, stop"; exit 1; fi
trap 'cp $SP/nvmev_backup/nvmev.ko.flag_opt $KO; echo "$(date) restored flag-bit ko"' EXIT
cp $SP/nvmev_backup/nvmev.ko.before $KO
echo "$(date) B: pre-change module start"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_aged_flag_ab_old \
  WORKLOADS="fio_mix70_d" POLICIES="proposed" tools/apps/run_all.sh
echo "$(date) A/B done"
