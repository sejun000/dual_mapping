#!/bin/bash
# (2026-10-19) FAST on the 128K-aligned QD32 replay. FAST exports a 96334774272 B namespace (158 MiB smaller than the
# others), so the shared traces were refused; a128_*_fast traces are the same generator/seed at FAST's capacity.
# Then a YCSB-A re-run of proposed_b12r (latency check) into results/final_rp_spillrp_rerun.
cd /home/sejun000/dual_mapping
while pgrep -f '^/bin/bash results/nvmevirt/q_rp_then_a128.sh' >/dev/null; do sleep 60; done
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
A=/home/sejun000/dual_mapping/results/final_rp_a128
for cfg in "50 70" "50 30" "90 70" "90 30"; do set -- $cfg; t=a128_r$1_s$2
  idle; echo "$(date) replay ${t}_fast : fast"
  TRACE=${t}_fast TRACE_DIR=/mnt/nvme_mounted/dm_trace_synth RESULTS=$A/replay_$t tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "fast:cache_bytes=B12"
done
idle; echo "$(date) ycsba rerun proposed_b12r"
PREFILL_SEQ=1 RESULTS_ROOT=/home/sejun000/dual_mapping/results/final_rp_spillrp_rerun/apps WORKLOADS="ycsba_d_t64" POLICIES="proposed_b12r" \
  tools/apps/run_all.sh >> /home/sejun000/dual_mapping/results/nvmevirt/q_fast_a128_apps.log 2>&1
echo "$(date) all done"
