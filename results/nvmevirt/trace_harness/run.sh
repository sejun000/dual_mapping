#!/bin/bash
# Userspace dm-harness replays (same FTL cores as the NVMeVirt modules, CPU only) of the sampled traces WITH reads, to
# get the map-read counts of the cache-based FTLs on the measured half (for the NVMeVirt time estimate and as a
# reference for the device runs). Geometry = the NVMeVirt device for each trace (L, P from tools/nvmev_geom.py).
cd /home/sejun000/dual_mapping
D=/mnt/nvme_mounted/dm_trace_sampled; O=results/nvmevirt/trace_harness
for t in spfresh kv_w1 ali1 ali3 ali2 ssd; do
  CAP=$(python3 -c "import json;print(json.load(open('$D/$t.json'))['capacity_bytes'])")
  M=$(python3 tools/nvmev_geom.py --fit $CAP)
  read L P < <(python3 -c "import sys;sys.path.insert(0,'tools');from nvmev_geom import geom,parse;g=geom(parse('$M'));print(g['L'],g['P'])")
  B=$((L * 4 * 1875 / 10000)); C="trace=$D/$t.csv capacity=$((L * 4096)) op=0.07 P=$P reads=1"
  taskset -c 50-92:2 ~/nvmevirt_dftl/dm/harness/replay_dftl $C params=cache_bytes=$B,dftl_tpbuf=1 > $O/${t}_dftl.json 2> $O/${t}_dftl.log &
  taskset -c 50-92:2 ~/nvmevirt_leaftl/dm/harness/replay_leaftl $C params=cache_bytes=$B > $O/${t}_leaftl.json 2> $O/${t}_leaftl.log &
  taskset -c 50-92:2 ~/nvmevirt_mu/dm/harness/replay_mu $C params=cache_bytes=$B > $O/${t}_mu.json 2> $O/${t}_mu.log &
done
wait
