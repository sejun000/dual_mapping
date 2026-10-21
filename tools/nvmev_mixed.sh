#!/bin/bash
# Mixed read/write test on the NVMeVirt device: 4 KiB random I/O, zipf(THETA) over the whole device, READ% reads, QD32.
#   tools/nvmev_mixed.sh <policy-tag>      env: THETA (0.9) READ (50) MEASURE_S (600) WARM_X (1.0)
# Phases: sequential precondition (1x) -> warm-up with the same mixed workload until WARM_X x capacity has been
# written (reaches GC steady state, not measured) -> measured run of MEASURE_S seconds with per-op latency
# percentiles for reads and writes. Device FTL counters are captured around the measured run (NVMe flush prints them).
set -e
TAG=$1; THETA=${THETA:-0.9}; READ=${READ:-50}; MEASURE_S=${MEASURE_S:-600}; WARM_X=${WARM_X:-1.0}
OUT=/home/sejun000/dual_mapping/results/nvmevirt/$TAG; mkdir -p $OUT
DEV=$(lsblk -dn -o NAME,MODEL | awk '$2 ~ /CSL_Virt/ {print "/dev/"$1}')
[ -n "$DEV" ] || { echo "no NVMeVirt device"; exit 1; }
MODEL=$(lsblk -dn -o MODEL $DEV); [[ "$MODEL" == CSL_Virt* ]] || { echo "refusing: $DEV is $MODEL"; exit 1; }
SIZE=$(sudo blockdev --getsize64 $DEV)
WARM_IO=$(python3 -c "print(int($SIZE * $WARM_X * 100 / (100 - $READ)) // 4096 * 4096)")   # total I/O so that writes = WARM_X x capacity
{ echo "device $DEV ($MODEL), $((SIZE>>30)) GiB; zipf:$THETA read=$READ% QD32 4K; warm writes ${WARM_X}x; measure ${MEASURE_S}s"
  sudo dmesg | grep "dm_ftl policy" | tail -1; } | tee $OUT/info.txt
COMMON="--filename=$DEV --direct=1 --ioengine=libaio --bs=4k --iodepth=32 --rw=randrw --rwmixread=$READ --random_distribution=zipf:$THETA --norandommap --randrepeat=0"
sudo fio --name=precond --filename=$DEV --direct=1 --ioengine=libaio --rw=write --bs=128k --iodepth=32 --size=100% --output-format=json --output=$OUT/precond.json
sudo fio --name=warm $COMMON --io_size=$WARM_IO --output-format=json --output=$OUT/warm.json
sudo nvme flush $DEV -n 1 >/dev/null 2>&1 || true; sudo dmesg | grep "\[dm flush\]" | tail -1 > $OUT/stats_before.txt
sudo fio --name=mixed $COMMON --time_based --runtime=$MEASURE_S --percentile_list=50:90:99:99.9:99.99 \
     --log_avg_msec=1000 --write_lat_log=$OUT/mixed --write_iops_log=$OUT/mixed --output-format=json --output=$OUT/mixed.json
sudo nvme flush $DEV -n 1 >/dev/null 2>&1 || true; sudo dmesg | grep "\[dm flush\]" | tail -1 > $OUT/stats_after.txt
python3 - $OUT <<'PY'
import json, sys, re
o = sys.argv[1]; j = json.load(open(o + '/mixed.json'))['jobs'][0]
for k in ('read', 'write'):
    x = j[k]; p = x['clat_ns']['percentile']
    print(f"{k:5s}: IOPS={x['iops']:8.0f}  p50={p['50.000000']/1e3:8.1f}us  p99={p['99.000000']/1e3:9.1f}us  p99.9={p['99.900000']/1e3:9.1f}us  max={x['clat_ns']['max']/1e6:8.1f}ms")
def st(f):
    s = open(f).read()
    m = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', s)}
    r = re.search(r'reads\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+)\)', s)
    g = re.search(r'\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+) other (\d+)\)', s)
    if r: m['rmap'] = int(r.group(5)); m['rhost'] = int(r.group(1))
    if g: m['pmap'] = int(g.group(5))
    return m
try:
    a, b = st(o + '/stats_before.txt'), st(o + '/stats_after.txt')
    hw = b['submitted'] - a['submitted']; hr = b['host_read_pages'] - a['host_read_pages']
    print(f"device: WAF={(b['prog']-a['prog'])/max(1,hw):.3f}  map reads per host read+write page={(b['rmap']-a['rmap'])/max(1,hw+hr):.3f}  map programs per host write page={(b['pmap']-a['pmap'])/max(1,hw):.3f}")
except Exception as e:
    print('device counters unavailable:', e)
PY
