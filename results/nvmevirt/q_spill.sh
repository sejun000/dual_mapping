#!/bin/bash
# Proposed + spill=2 (2026-10-19): a full cuckoo2 bucket pair spills the victim IU's fine map (and, spill=2, every other
# table-resident IU of the same 4 KiB flash translation page) to NAND instead of folding it; GTD 4 B per FTP charged to
# the 12.5% mapping-DRAM budget. Kernel core: ~/nvmevirt_proposed/dm/ftl_proposed.c (bit-identical to ftlsim spill=2).
# ONLY proposed, 11 paper workloads, same drivers / settings as results/nvmevirt/q_final_rp2.sh + q_leaftl_stage.sh:
# apps YCSB-A/B/C/E + TPC-C (aged, PREFILL_SEQ=1), raw fio R50/R90 x 4K-heavy/128K-heavy, traces Ali-3 and SPFresh.
# Results: results/final_rp_spill/{apps,fio,trace_ali3,trace_spfresh}; compare with results/final_rp (proposed_b12f).
cd /home/sejun000/dual_mapping
busy() { pgrep -f '^/bin/bash (/home/sejun000/dual_mapping/)?tools/(apps/run_all|nvmev_cycle)\.sh' >/dev/null || lsmod | grep -q '^nvmev '; }
idle() { while busy; do sleep 60; done; }
R=/home/sejun000/dual_mapping/results/final_rp_spill
mkdir -p $R
V="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1"
S="proposed:$V,map_budget_ppm=125000,onefront_fix=1,pool_cap=4,spill=2"
idle; echo "$(date) apps"
PREFILL_SEQ=1 RESULTS_ROOT=$R/apps WORKLOADS="ycsba_d_t64 ycsbb_d_t64 ycsbc_d_t64 ycsbe_d_t64 tpcc_d_t64" POLICIES="proposed_b12s" \
  tools/apps/run_all.sh >> $R/apps_run_all.log 2>&1
for cfg in "50 70" "50 30" "90 70" "90 30"; do set -- $cfg
  idle; echo "$(date) fio read=$1% 4K:128K=$2:$((100-$2))"
  READ=$1 BSSPLIT=4k/$2:128k/$((100-$2)) OUTROOT=$R/fio tools/nvmev_cycle.sh tools/nvmev_mixbs.sh _r$1_s$2 "$S"
done
D=/mnt/nvme_mounted/dm_trace_sampled
for TRACE in ali3 spfresh; do
  idle
  CAP=$(python3 -c "import json;print(json.load(open('$D/$TRACE.json'))['capacity_bytes'])")
  M=$(python3 tools/nvmev_geom.py --fit $CAP)
  echo "$(date) trace $TRACE memmap_size=$M"
  TRACE=$TRACE RESULTS=$R/trace_$TRACE NVMEV_MEMMAP_SIZE=$M tools/nvmev_cycle.sh tools/nvmev_trace.sh "" "$S"
done
echo "$(date) all done"
