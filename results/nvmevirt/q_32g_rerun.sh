#!/bin/bash
# (2026-11-13, user) rerun Page on TPC-C: its blk-layer mean read latency (980 us) is 4x the device-stat mean (246 us).
cd /home/sejun000/dual_mapping
export NVMEV_MEMMAP_SIZE=35073M NVMEV_BLKS=274
R=/home/sejun000/dual_mapping/results/final_32g/rerun
echo "$(date) tpcc page4k_g32"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="tpcc_d_t64" POLICIES="page4k_g32" tools/apps/run_all.sh >> $R/apps.run_all.log 2>&1
echo "$(date) all done"
