#!/bin/bash
# (2026-11-11) One device size for every workload: the 44G app device (host-visible L = 10798160 pages = 44.2 GB,
# FAST/CoMap L_fast = 10779648). Traces: SPFresh unsampled (40980 x 1 MiB regions), Alibaba ali3 at 17/1000, synthetic raw
# I/O 50/70/90% reads (70% 4K) at the 44G capacity. -> /mnt/nvme_mounted/dm_trace_44g
cd /home/sejun000/dual_mapping; O=/mnt/nvme_mounted/dm_trace_44g
for r in 50 70 90; do
  python3 tools/gen_synth_trace.py $O/a128_r${r}_s70 44229263360 $r 70 &
  python3 tools/gen_synth_trace.py $O/a128_r${r}_s70_fast 44153438208 $r 70 &
done
cd $O
/home/sejun000/dual_mapping/tools/trace_prep /mnt/nvme_mounted/alibaba_dwpd01to1.trace ali3.csv region=1048576 reads=1 order=touch sample=17/1000 > ali3.json 2> ali3.err &
/home/sejun000/dual_mapping/tools/trace_prep /mnt/nvme_mounted/spfresh_trace/spf.csv spfresh.csv region=1048576 reads=1 order=touch sample=1000/1000 > spfresh.json 2> spfresh.err &
wait; echo "$(date) gen done"
