#!/bin/bash
# Part C3: CPU time of each policy's FTL core (userspace harness built from scratch copies of ~/nvmevirt_<policy>/dm,
# replay.c + per-call clock_gettime timing of dm_policy.write / dm_policy.read in the measurement phase; timer overhead
# subtracted). One pinned core (CPU ${CPU:-13}, NUMA node 1 memory), runs sequential.
H=/tmp/claude-1001/-home-sejun000-dual-mapping/64054dd3-9fa1-4be8-986a-b7744c0b8ecf/scratchpad/harness
OUT=/home/sejun000/dual_mapping/results/cpu_cost
CPU=${CPU:-13}; NODE=${NODE:-1}
P3="U=16,fine_extra=0,fine_quota_ppm=42900,fine_full=1,fine_policy_cost_cold=1,gc_select_cost=1,cgc_vict_cb=1,coarse_class=2,adapt=2,uni_onefront=1,rec_block=1,fine_index_cuckoo2=1,map_budget_ppm=187500"
run() {   # tag workload-args...
  local tag=$1 pol=$2; shift 2
  /usr/bin/numactl --physcpubind=$CPU --membind=$NODE $H/tr_$pol "$@" > $OUT/$tag.json 2> $OUT/$tag.log
  echo "$(date +%T) $tag $(grep -o '"ns_per_write_page": [0-9.]*\|"ns_per_read_page": [0-9.]*' $OUT/$tag.json | tr '\n' ' ')"
}
for wl in ${WLS:-ycsbb skew}; do
  case $wl in
    ycsbb) W="trace=/mnt/nvme_mounted/dm_apptrace/ycsbb_d_t64/trace.csv capacity=44229263360 P=30976 op=0.070001602125 warmup=0.38 reads=1"; B=8098620 ;;
    skew)  W="trace=/mnt/nvme_mounted/dm_validate/skew.csv capacity=8G op=0.07 reads=1"; B=$((2097152 * 3 / 4)) ;;
    ali1)  W="trace=/mnt/nvme_mounted/dm_c2/ali1r.csv capacity=276605960192 P=197632 op=0.07 reads=1"; B=$((276605960192 / 4096 * 3 / 4)) ;;
  esac
  run ${wl}_proposed proposed $W U=16 "params=$P3"
  run ${wl}_page4k page4k $W U=16
  run ${wl}_dftl dftl $W U=16 "params=cache_bytes=$B,dftl_tpbuf=1"
  run ${wl}_leaftl leaftl $W U=16 "params=cache_bytes=$B"
  run ${wl}_iurmw iurmw $W U=16
  [[ $wl != ali1 ]] && run ${wl}_mu mu $W U=16 "params=cache_bytes=$B"
  [[ $wl != ali1 ]] && run ${wl}_fast fast $W U=16
done
