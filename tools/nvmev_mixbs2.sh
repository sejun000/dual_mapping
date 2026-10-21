#!/bin/bash
# Raw-device mixed workload on the NVMeVirt namespace with size-aligned requests: uniform random over the whole
# device, read:write READ%, 4 KiB requests aligned to 4 KiB and 128 KiB requests aligned to 128 KiB, mixed by
# request count MIX (4K:128K, e.g. 50:50) through fio token flow control (two jobs, same flow_id), total QD32.
#   tools/nvmev_mixbs2.sh <policy-tag>     env: READ (70) MIX (50:50) MEASURE_S (600) WARM_X (1.0) OUTROOT
# Phases: sequential 128K precondition (1x) -> warm-up with the same workload until WARM_X x capacity has been
# written (not measured) -> measured run of MEASURE_S seconds. FTL counters via the dm_dump sysfs trigger.
# fio 3.36 flow control occasionally stalls at start-up (both jobs at 1 I/O); every phase is checked and retried.
set -e
TAG=$1; READ=${READ:-70}; MIX=${MIX:-50:50}; MEASURE_S=${MEASURE_S:-600}; WARM_X=${WARM_X:-1.0}
F4=${MIX%%:*}; F128=${MIX##*:}
OUT=${OUTROOT:-/home/sejun000/dual_mapping/results/nvmevirt/mixbs_a128}/$TAG; mkdir -p $OUT
DEV=$(lsblk -dn -o NAME,MODEL | awk '$2 ~ /CSL_Virt/ {print "/dev/"$1}')
[ -n "$DEV" ] || { echo "no NVMeVirt device"; exit 1; }
[ "$(echo "$DEV" | wc -l)" = 1 ] || { echo "more than one CSL_Virt device"; exit 1; }
MODEL=$(lsblk -dn -o MODEL $DEV); [[ "$MODEL" == CSL_Virt* ]] || { echo "refusing: $DEV is $MODEL"; exit 1; }
case "$(basename $DEV)" in nvme0n1|nvme1n1|nvme2n1|sda*) echo "refusing protected disk $DEV"; exit 1;; esac
[ -z "$(lsblk -n -o MOUNTPOINT $DEV | tr -d '[:space:]')" ] || { echo "refusing: $DEV is mounted"; exit 1; }
SIZE=$(sudo blockdev --getsize64 $DEV)
# warm-up: total bytes (reads+writes) per job so that writes reach WARM_X x capacity, split by each job's byte share
read W4 W128 < <(python3 -c "
s,wx,rd,f4,f128=$SIZE,$WARM_X,$READ,$F4,$F128
tot=s*wx*100/(100-rd); b4=f4*4096; b128=f128*131072
print(int(tot*b4/(b4+b128))//4096*4096, int(tot*b128/(b4+b128))//131072*131072)")
dump() { echo 1 | sudo tee /sys/module/nvmev/parameters/dm_dump >/dev/null; sleep 1; sudo dmesg | grep "\[dm flush\]" | tail -1; }
{ echo "device $DEV ($MODEL), $((SIZE>>30)) GiB; uniform random, read=$READ%, 4K:128K=$MIX by count (size-aligned), QD32 (16+16); warm writes ${WARM_X}x; measure ${MEASURE_S}s"
  sudo dmesg | grep "dm_ftl policy" | tail -1; } | tee $OUT/info.txt
G="--filename=$DEV --direct=1 --ioengine=libaio --rw=randrw --rwmixread=$READ --random_distribution=random --norandommap --randrepeat=0"
J4="--name=s4k --bs=4k --blockalign=4k --iodepth=16 --flow_id=1 --flow=$F4 --flow_sleep=10"
J128="--name=s128k --bs=128k --blockalign=128k --iodepth=16 --flow_id=1 --flow=$F128 --flow_sleep=10"
[ "$F4" = 0 ] && J4=""; [ "$F128" = 0 ] && J128=""
ok() { python3 -c "import json,sys;j=json.load(open('$1'))['jobs'];sys.exit(0 if sum(x['read']['total_ios']+x['write']['total_ios'] for x in j)>1000 else 1)"; }
run() {   # $1 output json, rest: fio args; retry on the start-up stall
  local out=$1; shift
  for a in 1 2 3; do sudo fio $G "$@" --output-format=json --output=$out && ok $out && return 0; echo "fio stalled/failed (attempt $a), retrying"; done
  return 1
}
sudo fio --name=precond --filename=$DEV --direct=1 --ioengine=libaio --rw=write --bs=128k --iodepth=32 --size=100% --output-format=json --output=$OUT/precond.json
run $OUT/warm.json $J4 ${J4:+--io_size=$W4} $J128 ${J128:+--io_size=$W128}
dump > $OUT/stats_before.txt
run $OUT/mixbs.json --time_based --runtime=$MEASURE_S --percentile_list=50:90:99:99.9:99.99 $J4 $J128
dump > $OUT/stats_after.txt
python3 - $OUT <<'PY' | tee $OUT/summary.txt
import json, sys, re
o = sys.argv[1]; jobs = json.load(open(o + '/mixbs.json'))['jobs']
tot = 0; mb = 0; n = {}
for j in jobs:
    for k in ('read', 'write'):
        x = j[k]; p = x['clat_ns'].get('percentile', {}); tot += x['iops']; mb += x['bw_bytes'] / 1e6
        n[j['jobname']] = n.get(j['jobname'], 0) + x['total_ios']
        if x['total_ios']:
            print(f"{j['jobname']:5s} {k:5s}: IOPS={x['iops']:8.0f} MB/s={x['bw_bytes']/1e6:7.1f} p50={p.get('50.000000',0)/1e3:8.1f}us p99={p.get('99.000000',0)/1e3:9.1f}us p99.9={p.get('99.900000',0)/1e3:9.1f}us max={x['clat_ns']['max']/1e6:8.1f}ms")
print(f"total IOPS={tot:.0f}  MB/s={mb:.1f}  request mix (4K:128K) = {n}")
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
