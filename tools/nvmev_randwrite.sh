#!/bin/bash
# Full-random-write test on the NVMeVirt device: sequential precondition (1x), then 4K random write QD32 for 2x capacity.
#   tools/nvmev_randwrite.sh <policy-tag>   (the loaded nvmev.ko decides the policy; the tag only names outputs)
set -e
TAG=$1; OUT=/home/sejun000/dual_mapping/results/nvmevirt/$TAG; mkdir -p $OUT
DEV=$(lsblk -dn -o NAME,MODEL | awk '$2 ~ /CSL_Virt/ {print "/dev/"$1}')
[ -n "$DEV" ] || { echo "no NVMeVirt device"; exit 1; }
MODEL=$(lsblk -dn -o MODEL $DEV); [[ "$MODEL" == CSL_Virt* ]] || { echo "refusing: $DEV is $MODEL"; exit 1; }
SIZE=$(sudo blockdev --getsize64 $DEV)
echo "device $DEV ($MODEL), $((SIZE>>30)) GiB" | tee $OUT/info.txt
sudo dmesg | grep "dm_ftl policy" | tail -1 | tee -a $OUT/info.txt
sudo fio --name=precond --filename=$DEV --direct=1 --ioengine=libaio --rw=write --bs=128k --iodepth=32 --size=100% --output-format=json --output=$OUT/precond.json
sudo nvme flush $DEV -n 1 >/dev/null 2>&1 || true; sudo dmesg | grep "\[dm flush\]" | tail -1 > $OUT/stats_after_precond.txt
sudo fio --name=randw --filename=$DEV --direct=1 --ioengine=libaio --rw=randwrite --bs=4k --iodepth=32 --io_size=$((SIZE*2)) --norandommap --randrepeat=0 \
     --percentile_list=50:90:99:99.9:99.99 --log_avg_msec=1000 --write_bw_log=$OUT/randw --write_lat_log=$OUT/randw --write_iops_log=$OUT/randw \
     --output-format=json --output=$OUT/randw.json
sudo nvme flush $DEV -n 1 >/dev/null 2>&1 || true; sudo dmesg | grep "\[dm flush\]" | tail -1 > $OUT/stats_after_randw.txt
python3 - $OUT <<'PY'
import json,sys,re
o=sys.argv[1]; j=json.load(open(o+'/randw.json'))['jobs'][0]['write']
p=j['clat_ns']['percentile']
print(f"randwrite 4K QD32: IOPS={j['iops']:.0f} BW={j['bw']/1024:.1f} MiB/s  clat p50={p['50.000000']/1e3:.1f}us p99={p['99.000000']/1e3:.1f}us p99.9={p['99.900000']/1e3:.1f}us")
def st(f):
    s=open(f).read(); m=dict(re.findall(r'(\w+)=(\d+)', s)); return m
a=st(o+'/stats_after_precond.txt'); b=st(o+'/stats_after_randw.txt')
if a and b:
    dh=int(b['submitted'])-int(a['submitted']); dp=int(b['prog'])-int(a['prog'])
    print(f"device WAF during randwrite = {dp/dh:.3f} (host pages {dh}, NAND programs {dp})")
PY
