#!/bin/bash
# Size-aligned uniform random (2026-10-19), replacing q_a128.sh: fio flow control (two jobs, one flow_id) ran at an
# effective QD of ~5, so latency/throughput were not comparable. Same workloads are now synthetic traces
# (tools/gen_synth_trace.py: 4K at 4K, 128K at 128K alignment, uniform over the device, 2x capacity of writes)
# replayed closed-loop at QD32 by tools/trace_replay: seq precondition -> first 1x capacity of writes (warm-up)
# -> second 1x (measured). R50/R90 x 4K:128K = 70:30 / 30:70; OP 7%, 12.5% mapping DRAM; proposed = spill=2.
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_rp_a128
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
S="proposed:$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2"
run() { local t=a128_r$1_s$2; shift 2
  idle; echo "$(date) replay $t : ${*%%:*}"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$R/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$@"
}
for cfg in "50 70" "50 30" "90 70" "90 30"; do run $cfg page4k "$S"; done
for cfg in "50 70" "50 30" "90 70" "90 30"; do
  run $cfg "leaftl:cache_bytes=B12,leaftl_batch=128" "dftl:cache_bytes=B12,dftl_tpbuf=1" "iurmw:U=8" "fast:cache_bytes=B12" "mu:cache_bytes=B12"
done
echo "$(date) all done"
