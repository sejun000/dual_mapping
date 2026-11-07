#!/bin/bash
# (2026-11-06) Fine-map size over time: final DUET on ~/nvmevirt_proposedst (instrumented, decisions unchanged) for the three
# ablation workloads, with a poller that triggers dm_dump every 5 s and logs "<unix time> [dm flush] ... fine_blocks= fpages=
# spages= ..." to <out>/ts.log. dm_dump only prints counters. Starts after q_fastfull.sh. -> results/final_ts
cd /home/sejun000/dual_mapping
while pgrep -f "^/bin/bash (/home/sejun000/dual_mapping/)?results/nvmevirt/q_fastfull\.sh" >/dev/null; do sleep 60; done
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_ts; mkdir -p $R
DU="U=16,fine_extra=0,fine_quota_ppm=48300,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=0,adapt=2,uni_onefront=0,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=125476,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"
poll() {   # $1 = log file; runs until killed
  while true; do
    if [ -e /sys/module/nvmev/parameters/dm_dump ]; then
      echo 1 | sudo tee /sys/module/nvmev/parameters/dm_dump >/dev/null 2>&1
      l=$(sudo dmesg | grep '\[dm flush\].*smp_n=' | tail -1); [ -n "$l" ] && echo "$(date +%s) $l" >> "$1"
    fi
    sleep 5
  done
}
run() {    # $1 = out dir, rest = command
  local o=$1; shift; mkdir -p $o; poll $o/ts.log & local pp=$!
  "$@"; kill $pp; wait $pp 2>/dev/null
}
D=/mnt/nvme_mounted/dm_trace_sampled
idle; CAP=$(python3 -c "import json;print(json.load(open('$D/ali3.json'))['capacity_bytes'])"); M=$(python3 tools/nvmev_geom.py --fit $CAP)
echo "$(date) trace ali3"
TRACE=ali3 RESULTS=$R/trace_ali3 NVMEV_MEMMAP_SIZE=$M run $R/trace_ali3 tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposedst:$DU"
idle; echo "$(date) apps ycsba"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64" POLICIES="proposedst_vb" \
  run $R/apps/ycsba_d_t64 tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
t=a128_r50_s70; idle; echo "$(date) replay $t"
TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t \
  run $R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "proposedst:$DU"
echo "$(date) all done"
