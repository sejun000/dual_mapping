#!/bin/bash
# Real-trace replay on the NVMeVirt device (closed loop, QD 32), for use under tools/nvmev_cycle.sh:
#   TRACE=<name> NVMEV_MEMMAP_SIZE=<from tools/nvmev_geom.py --fit> tools/nvmev_cycle.sh tools/nvmev_trace.sh "" <policies...>
#   tools/nvmev_trace.sh <policy-tag>     env: TRACE (required: spfresh kv_w1 ali1 ali2 ali3 ssd)
#        TRACE_DIR (/mnt/nvme_mounted/dm_trace_sampled)  QD (32)  MAX_HOURS (5: replay stops there, marked truncated)
#        REPLAY_CPU (92)  RESULTS (results/nvmevirt/trace_<TRACE>)
#        TEST_LOOP=/dev/loopN: testing only -- replay on that loop device instead of NVMeVirt; the hook then never
#        touches nvmev and fakes the counter line from the loop device's /sys/block/<dev>/stat (WAF 1 by construction).
# Input: <TRACE_DIR>/<TRACE>.csv (+ .json) from tools/trace_prep ... reads=1 order=touch sample=k/1000.
# Phases as ftlsim's trace mode: sequential precondition of the whole device (1x, 128 KiB writes) -> first 50% of the
# trace's write pages (warm-up, not measured) -> rest of the trace (measured). The device FTL counters are dumped
# (dm_dump) at both boundaries; device WAF and map reads are taken over the measured half.
# Output: RESULTS/<tag>/{info.txt,replay.log,summary.json,lat_hist.csv,iops.csv,stats_*.txt,result.txt}; one line per
# policy is appended to RESULTS/SUMMARY.txt.
set -e
if [ "$1" = "--hook" ]; then        # called by trace_replay (as root): --hook <outdir> [test-loop-dev] <phase>
    O=$2; PH=${@: -1}; TL=""; [ $# -eq 4 ] && TL=$3
    if [ -n "$TL" ]; then
        read -r rd _ rs _ wr _ ws _ < /sys/block/${TL#/dev/}/stat
        echo "[test flush] policy=loop submitted=$((ws / 8)) prog=$((ws / 8)) (host $((ws / 8)) gc 0 rmw 0 fold 0 map 0 other 0) reads(host $((rs / 8)) gc 0 rmw 0 fold 0 map 0) host_read_pages=$((rs / 8)) erases=0 map_miss=0 mem=0" > "$O/stats_$PH.txt"
    else
        echo 1 > /sys/module/nvmev/parameters/dm_dump
        dmesg | grep "\[dm flush\]" | tail -1 > "$O/stats_$PH.txt"
    fi
    echo "$(date '+%F %T') $PH" >> "$O/phases.txt"
    exit 0
fi
TAG=$1
[ -n "$TAG" ] || { echo "usage: TRACE=<name> $0 <policy-tag>"; exit 1; }
[ -n "$TRACE" ] || { echo "TRACE not set"; exit 1; }
ROOT=/home/sejun000/dual_mapping
TRACE_DIR=${TRACE_DIR:-/mnt/nvme_mounted/dm_trace_sampled}
CSV=$TRACE_DIR/$TRACE.csv; META=$TRACE_DIR/$TRACE.json
QD=${QD:-32}; MAX_HOURS=${MAX_HOURS:-5}; CPU=${REPLAY_CPU:-92}
RES=${RESULTS:-$ROOT/results/nvmevirt/trace_$TRACE}
OUT=$RES/$TAG; mkdir -p "$OUT"
[ -s "$CSV" ] && [ -s "$META" ] || { echo "missing $CSV / $META"; exit 1; }
if [ -n "$TEST_LOOP" ]; then
    [[ "$TEST_LOOP" =~ ^/dev/loop[0-9]+$ ]] || { echo "TEST_LOOP must be /dev/loopN"; exit 1; }
    DEV=$TEST_LOOP; MODEL="loop (TEST)"; LOOPARGS="--allow-loop"
else
    DEV=$(lsblk -dn -o NAME,MODEL | awk '$2 ~ /CSL_Virt/ {print "/dev/"$1}')
    [ -n "$DEV" ] || { echo "no NVMeVirt device"; exit 1; }
    [ $(echo "$DEV" | wc -w) -eq 1 ] || { echo "more than one CSL_Virt device: $DEV"; exit 1; }
    MODEL=$(lsblk -dn -o MODEL $DEV); [[ "$MODEL" == CSL_Virt* ]] || { echo "refusing: $DEV is $MODEL"; exit 1; }
    LOOPARGS=""
fi
SIZE=$(sudo blockdev --getsize64 $DEV)
CAP=$(python3 -c "import json;print(json.load(open('$META'))['capacity_bytes'])")
[ "$SIZE" -ge "$CAP" ] || { echo "device $SIZE B smaller than trace capacity $CAP B (set NVMEV_MEMMAP_SIZE)"; exit 1; }
{ echo "device $DEV ($MODEL), $SIZE B ($((SIZE>>20)) MiB); trace $TRACE: $CSV, capacity $CAP B; QD$QD; max ${MAX_HOURS}h"
  [ -n "$TEST_LOOP" ] || sudo dmesg | grep "dm_ftl policy" | tail -1
  python3 -c "import json;d=json.load(open('$META'));print('trace:', {k:d[k] for k in ('sample','kept_regions','capacity_gib','write_gib','read_gib','writes','reads','turnovers')})"
} | tee $OUT/info.txt
rm -f $OUT/stats_*.txt $OUT/phases.txt
sudo taskset -c $CPU $ROOT/tools/trace_replay --dev $DEV --trace $CSV --out $OUT --qd $QD $LOOPARGS \
     --max-seconds $((MAX_HOURS * 3600)) --hook "$ROOT/tools/nvmev_trace.sh --hook $OUT ${TEST_LOOP:+$TEST_LOOP}" 2>&1 | tee $OUT/replay.log
sudo chown -R $(id -u):$(id -g) $OUT
python3 - $OUT $TRACE $TAG $RES/SUMMARY.txt <<'PY'
import json, sys, re, os
o, trace, tag, summ = sys.argv[1:5]
j = json.load(open(o + '/summary.json'))
def st(f):
    s = open(f).read()
    m = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', s)}
    r = re.search(r'reads\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+)\)', s)
    g = re.search(r'\(host (\d+) gc (\d+) rmw (\d+) fold (\d+) map (\d+) other (\d+)\)', s)
    if r: m['rmap'] = int(r.group(5)); m['rgc'] = int(r.group(2)) + int(r.group(3)) + int(r.group(4))
    if g: m['pmap'] = int(g.group(5))
    return m
dev = {}
try:
    a, b = st(o + '/stats_warm_end.txt'), st(o + '/stats_end.txt')
    hw = b['submitted'] - a['submitted']; hr = b['host_read_pages'] - a['host_read_pages']
    dev = dict(host_write_pages=hw, host_read_pages=hr, prog=b['prog'] - a['prog'], WAF=(b['prog'] - a['prog']) / max(1, hw),
               map_reads=b['rmap'] - a['rmap'], map_reads_per_host_page=(b['rmap'] - a['rmap']) / max(1, hw + hr),
               map_progs_per_host_write_page=(b['pmap'] - a['pmap']) / max(1, hw), erases=b['erases'] - a['erases'],
               map_miss=b['map_miss'] - a['map_miss'], mem_bytes=b['mem'])
except Exception as e:
    dev = dict(error=str(e))
j['device'] = dev
json.dump(j, open(o + '/result.json', 'w'), indent=1)
m = j['measured']; r = j['read_lat_us']; w = j['write_lat_us']
lat = lambda h: f"p50={h['p50']:.0f} p99={h['p99']:.0f} p99.9={h['p99.9']:.0f} p99.99={h['p99.99']:.0f} max={h['max']:.0f}"
line = (f"{tag:9s} IOPS={m['iops']:8.0f} (R {m['read_iops']:8.0f} W {m['write_iops']:7.0f}; R {m['read_MiBps']:6.1f} W {m['write_MiBps']:6.1f} MiB/s) "
        f"WAF={dev.get('WAF', float('nan')):6.3f} mapR/pg={dev.get('map_reads_per_host_page', float('nan')):.3f} | "
        f"read_us {lat(r)} | write_us {lat(w)} | {j['total_s']/3600:.2f}h" + (" TRUNCATED" if j['truncated'] else ""))
open(o + '/result.txt', 'w').write(line + '\n')
new = not os.path.exists(summ)
with open(summ, 'a') as f:
    if new: f.write(f"# {trace}: NVMeVirt closed-loop QD{j['qd']} replay, measured = second 50% of write pages; latency in us\n")
    f.write(line + '\n')
print(line)
PY
