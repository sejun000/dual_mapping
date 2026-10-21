#!/bin/bash
# Raw fio with size-aligned requests (2026-10-19): 4K requests 4K-aligned, 128K requests 128K-aligned (tools/nvmev_mixbs2.sh,
# two fio jobs under one flow_id, QD 16+16). Same matrix as the 4K-aligned raw fio in results/final_rp/fio:
# R50/R90 x 4K:128K = 70:30 / 30:70, OP 7%, 12.5% mapping DRAM. Proposed = spill=2 version.
# Order: page4k + proposed for all four configs first, then the other five baselines.
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_rp_a128
mkdir -p $R/fio
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
S="proposed:$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2"
run() { local rd=$1 s=$2; shift 2
  idle; echo "$(date) fio a128 read=$rd% 4K:128K=$s:$((100-s)) : $*"
  READ=$rd MIX=$s:$((100-s)) OUTROOT=$R/fio tools/nvmev_cycle.sh tools/nvmev_mixbs2.sh _r${rd}_s$s "$@"
}
for cfg in "50 70" "50 30" "90 70" "90 30"; do run $cfg page4k "$S"; done
for cfg in "50 70" "50 30" "90 70" "90 30"; do
  run $cfg "leaftl:cache_bytes=B12,leaftl_batch=128" "dftl:cache_bytes=B12,dftl_tpbuf=1" "iurmw:U=8" "fast:cache_bytes=B12" "mu:cache_bytes=B12"
done
echo "$(date) all done"
