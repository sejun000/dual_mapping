#!/bin/bash
# (2026-10-21) Raw I/O R70 (70% reads) x 4K:128K = 70:30 / 30:70, 128K size-aligned synthetic traces at QD32 (same
# generator/seed as a128_r50/r90; FAST on its own-capacity trace), all 7 policies -> results/final_rp_a128/replay_a128_r70_*.
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
A=/home/sejun000/dual_mapping/results/final_rp_a128
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
S="proposed:$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2,spill_rp=1,spill_rp_clean=1"
for s in 70 30; do t=a128_r70_s$s
  idle; echo "$(date) replay $t"
  TRACE=$t TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$A/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" \
    page4k "$S" "leaftl:cache_bytes=B12,leaftl_batch=128" "dftl:cache_bytes=B12,dftl_tpbuf=1" "iurmw:U=8" "mu:cache_bytes=B12"
  idle; echo "$(date) replay ${t}_fast"
  TRACE=${t}_fast TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$A/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "fast:cache_bytes=B12"
done
echo "$(date) all done"
