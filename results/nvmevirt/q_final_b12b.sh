#!/bin/bash
# FINAL paper matrix on NVMeVirt, mapping DRAM = 12.5% (1/8) of the 4 B page map for every budget-configurable policy
# (proposed with onefront_fix=1 pool_cap=4, DFTL, LeaFTL, uFTL, FAST via a log-block cap); IU+RMW at 32 KiB IU (= 1/8); page4k = 100%.
# Patched admin.c in all modules. Results: results/final_b12/{apps,trace_<t>,fio}
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
P12F="$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4"
POL=(page4k "proposed:$P12F" "dftl:cache_bytes=B12,dftl_tpbuf=1" "leaftl:cache_bytes=B12" "fast:cache_bytes=B12" "mu:cache_bytes=B12" "iurmw:U=8")
R=/home/sejun000/dual_mapping/results/final_b12

# apps: the run_all started by q_final_b12.sh keeps running (fast/iurmw now map to the 12.5% configs in common.sh);
# this queue waits for it, then re-runs run_all once to pick up anything not DONE.
idle; echo "$(date) 1) apps (aged device)"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps \
  WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbd_d_t64 ycsbe_d_t64 ycsbfstd_d_t64 tpcc_d_t64 vmhot1g" \
  POLICIES="page4k proposed_b12f dftl_b12 leaftl_b12 fast mu_b12 iurmw" tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1

D=/mnt/nvme_mounted/dm_trace_sampled
for TRACE in spfresh kv_w1; do
  idle
  CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])") || { echo "no $TRACE.json"; continue; }
  M=$(python3 tools/nvmev_geom.py --fit $CAP) || { echo "$TRACE does not fit"; continue; }
  echo "$(date) 2) trace $TRACE: capacity $CAP B, memmap_size=$M"
  TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "${POL[@]}"
done

for rd in 50 70 90; do for mx in 50 70 30; do
  idle; echo "$(date) 3) fio read=$rd% 4K:128K=$mx:$((100-mx))"
  READ=$rd BSSPLIT=4k/$mx:128k/$((100-mx)) OUTROOT=$R/fio tools/nvmev_cycle.sh tools/nvmev_mixbs.sh _r${rd}_s${mx} "${POL[@]}"
done; done
echo "$(date) all done"
