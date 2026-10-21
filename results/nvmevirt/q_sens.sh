#!/bin/bash
# NVMeVirt confirmation of the key sensitivity points (DAC'27 Part B), aged-device app condition (PREFILL_SEQ=1).
# Workloads ycsbb_d_t64 and fio_mix70_d. Variants (RESULTS_ROOT=results/apps_sens/<variant>):
#   b18 : page4k proposed dftl leaftl          (re-baseline under the same host conditions: governor left alone)
#   b10 : proposed_b10 dftl_b10 leaftl_b10      (mapping DRAM 10% of the page map: map_budget_ppm / cache_bytes)
#   b25 : proposed_b25 dftl_b25 leaftl_b25      (25%)
#   iu  : proposed_u8 proposed_u32             (32 KiB / 128 KiB IU at 18.75%; harness parity checked)
# CPU_GOVERNOR="": run_app.sh does not touch the CPU governor (the earlier apps_aged matrix set 'performance').
# Waits for: the device to be idle, the light simulator queue (PID in results/sens/light.pid) to finish, and
# results/nvmevirt/q_sens.hold to be absent (host-side CPU timing in progress).
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
# wait for the light simulator queue (results/sens/light.pid = its xargs); the 8 ali3 jobs may still run on node 0
SENSPID=$(cat results/sens/light.pid 2>/dev/null)
while [[ -n "$SENSPID" ]] && kill -0 "$SENSPID" 2>/dev/null; do sleep 60; done
while [[ -e results/nvmevirt/q_sens.hold ]]; do sleep 60; done
while busy; do sleep 60; done
echo "$(date) sensitivity queue start"
for v in b18 b10 b25 iu; do
  mkdir -p results/apps_sens/$v
  [[ -s results/apps_sens/$v/b18_44G.txt ]] || cp results/apps_aged/b18_44G.txt results/apps_sens/$v/b18_44G.txt
done
run() {   # $1 variant, $2 workload, $3.. policies
  local v=$1 wl=$2; shift 2
  while busy; do sleep 60; done
  echo "$(date) $v $wl: $*"
  CPU_GOVERNOR="" PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/apps_sens/$v WORKLOADS="$wl" \
    POLICIES="$*" tools/apps/run_all.sh
  echo "$(date) $v $wl done (rc=$?)"
}
for wl in fio_mix70_d ycsbb_d_t64; do
  run b10 $wl proposed_b10 dftl_b10 leaftl_b10
  run b25 $wl proposed_b25 dftl_b25 leaftl_b25
  run iu  $wl proposed_u8 proposed_u32
  run b18 $wl proposed dftl leaftl page4k
done
echo "$(date) sensitivity queue done"
