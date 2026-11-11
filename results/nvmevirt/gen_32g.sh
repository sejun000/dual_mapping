#!/bin/bash
# (2026-11-11) One device for every workload: 32 GiB host-visible (memmap 35073M, 274 x 128 MiB superblocks, OP 7%).
# Traces sized to 32 GiB = 8388608 pages (<= FAST/CoMap's whole-block L). Synthetic raw I/O 50/70/90% reads (70% 4K),
# Alibaba ali3 spatially sampled at 13/1000, SPFresh unsampled packed at 64 KiB and 512 KiB regions (pick the one that fits).
cd /home/sejun000/dual_mapping; O=/mnt/nvme_mounted/dm_trace_32g; TP=/home/sejun000/dual_mapping/tools/trace_prep
for r in 50 70 90; do python3 tools/gen_synth_trace.py $O/a128_r${r}_s70 34359738368 $r 70 & done
cd $O
$TP /mnt/nvme_mounted/alibaba_dwpd01to1.trace ali3.csv region=1048576 reads=1 order=touch sample=13/1000 > ali3.json 2> ali3.err &
$TP /mnt/nvme_mounted/spfresh_trace/spf.csv spfresh_r64k.csv region=65536 reads=1 order=touch sample=1000/1000 > spfresh_r64k.json 2> spfresh_r64k.err &
$TP /mnt/nvme_mounted/spfresh_trace/spf.csv spfresh_r512k.csv region=524288 reads=1 order=touch sample=1000/1000 > spfresh_r512k.json 2> spfresh_r512k.err &
wait; echo "$(date) gen done"
