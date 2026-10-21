#!/bin/bash
# Raw-device mixed workload on the NVMeVirt namespace: uniform random over the whole device, read:write READ%,
# request sizes mixed 4 KiB / 128 KiB by request count (BSSPLIT, fio bssplit syntax), QD32, O_DIRECT.
#   tools/nvmev_mixbs.sh <policy-tag>     env: READ (70) BSSPLIT (4k/50:128k/50) MEASURE_S (600) WARM_X (1.0) OUTROOT
# Phases: sequential 128K precondition (1x) -> warm-up with the same workload until WARM_X x capacity has been written
# (GC steady state, not measured) -> measured run of MEASURE_S seconds. Device FTL counters are captured around the
# measured run through the dm_dump sysfs trigger. Offsets are 4 KiB-aligned for every size (fio default), as file
# systems place large writes on 4 KiB boundaries.
set -e
TAG=$1; READ=${READ:-70}; BSSPLIT=${BSSPLIT:-4k/50:128k/50}; MEASURE_S=${MEASURE_S:-600}; WARM_X=${WARM_X:-1.0}
OUT=${OUTROOT:-/home/sejun000/dual_mapping/results/nvmevirt/mixbs}/$TAG; mkdir -p $OUT
DEV=$(lsblk -dn -o NAME,MODEL | awk '$2 ~ /CSL_Virt/ {print "/dev/"$1}')
[ -n "$DEV" ] || { echo "no NVMeVirt device"; exit 1; }
[ "$(echo "$DEV" | wc -l)" = 1 ] || { echo "more than one CSL_Virt device"; exit 1; }
MODEL=$(lsblk -dn -o MODEL $DEV); [[ "$MODEL" == CSL_Virt* ]] || { echo "refusing: $DEV is $MODEL"; exit 1; }
case "$(basename $DEV)" in nvme0n1|nvme1n1|nvme2n1|sda*) echo "refusing protected disk $DEV"; exit 1;; esac
[ -z "$(lsblk -n -o MOUNTPOINT $DEV | tr -d '[:space:]')" ] || { echo "refusing: $DEV is mounted"; exit 1; }
SIZE=$(sudo blockdev --getsize64 $DEV)
# average bytes written per request -> total I/O so that written bytes = WARM_X x capacity
WARM_IO=$(python3 - "$SIZE" "$WARM_X" "$READ" "$BSSPLIT" <<'PY'
import sys
size, wx, rd, sp = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
u = {'k': 1 << 10, 'm': 1 << 20}
def b(s): return int(s[:-1]) * u[s[-1].lower()]
print(int(size * wx * 100 / (100 - rd)) // 4096 * 4096)
PY
)
dump() { echo 1 | sudo tee /sys/module/nvmev/parameters/dm_dump >/dev/null; sleep 1; sudo dmesg | grep "\[dm flush\]" | tail -1; }
{ echo "device $DEV ($MODEL), $((SIZE>>30)) GiB; uniform random, read=$READ%, bssplit=$BSSPLIT, QD32; warm writes ${WARM_X}x; measure ${MEASURE_S}s"
  sudo dmesg | grep "dm_ftl policy" | tail -1; } | tee $OUT/info.txt
COMMON="--filename=$DEV --direct=1 --ioengine=libaio --iodepth=32 --rw=randrw --rwmixread=$READ --bssplit=$BSSPLIT --random_distribution=random --norandommap --randrepeat=0"
sudo fio --name=precond --filename=$DEV --direct=1 --ioengine=libaio --rw=write --bs=128k --iodepth=32 --size=100% --output-format=json --output=$OUT/precond.json
sudo fio --name=warm $COMMON --io_size=$WARM_IO --output-format=json --output=$OUT/warm.json
dump > $OUT/stats_before.txt
sudo fio --name=mixbs $COMMON --time_based --runtime=$MEASURE_S --percentile_list=50:90:99:99.9:99.99 \
     --log_avg_msec=1000 --write_lat_log=$OUT/mixbs --write_iops_log=$OUT/mixbs --output-format=json --output=$OUT/mixbs.json
dump > $OUT/stats_after.txt
python3 - $OUT <<'PY' | tee $OUT/summary.txt
import json, sys, re
o = sys.argv[1]; j = json.load(open(o + '/mixbs.json'))['jobs'][0]
tot = 0
for k in ('read', 'write'):
    x = j[k]; p = x['clat_ns']['percentile']; tot += x['iops']
    print(f"{k:5s}: IOPS={x['iops']:8.0f} MB/s={x['bw_bytes']/1e6:7.1f} p50={p['50.000000']/1e3:8.1f}us p99={p['99.000000']/1e3:9.1f}us p99.9={p['99.900000']/1e3:9.1f}us max={x['clat_ns']['max']/1e6:8.1f}ms")
print(f"total IOPS={tot:.0f}")
def st(f):
    s = open(f).read(); m = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', s)}
    r = re.search(r'reads\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+)\)', s)
    if r: m['rmap'] = int(r.group(5))
    return m
try:
    a, b = st(o + '/stats_before.txt'), st(o + '/stats_after.txt')
    hw = b['submitted'] - a['submitted']; hr = b['host_read_pages'] - a['host_read_pages']
    print(f"device: WAF={(b['prog']-a['prog'])/max(1,hw):.3f}  map reads per host page={(b['rmap']-a['rmap'])/max(1,hw+hr):.3f}")
except Exception as e:
    print('device counters unavailable:', e)
PY
