#!/usr/bin/env python3
"""Synthetic size-aligned uniform-random trace for tools/trace_replay (QD32 closed loop, via tools/nvmev_trace.sh).
Each request: size 4 KiB (prob F4) or 128 KiB, op R with prob READ%; offset uniform over the device, aligned to the
request's own size (4K at 4K, 128K at 128K). Stops when written pages reach TURN x capacity (the replayer measures the
second half). Writes <out>.csv (dev,op,byte_off,len) and <out>.json (metadata nvmev_trace.sh expects).
  gen_synth_trace.py OUT CAPACITY_BYTES READ_PCT F4_PCT [TURN=2] [SEED=1]"""
import json, sys
import numpy as np
out, cap, rd, f4 = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
turn = float(sys.argv[5]) if len(sys.argv) > 5 else 2.0
seed = int(sys.argv[6]) if len(sys.argv) > 6 else 1
L = cap // 4096; L -= L % 32
rng = np.random.default_rng(seed)
target = int(turn * L)
wp = rp = nw = nr = 0
CH = 1 << 20
with open(out + '.csv', 'w') as f:
    while wp < target:
        small = rng.random(CH) < f4 / 100
        isr = rng.random(CH) < rd / 100
        pg = np.where(small, rng.integers(0, L, CH), rng.integers(0, L // 32, CH) * 32)
        n = np.where(small, 1, 32)
        wn = np.cumsum(np.where(isr, 0, n))
        stop = np.searchsorted(wn, target - wp, side='left')
        k = min(CH, stop + 1)
        small, isr, pg, n = small[:k], isr[:k], pg[:k], n[:k]
        ops = np.where(isr, 'R', 'W')
        f.write(''.join(f'0,{o},{p * 4096},{m * 4096}\n' for o, p, m in zip(ops.tolist(), pg.tolist(), n.tolist())))
        wp += int(n[~isr].sum()); rp += int(n[isr].sum()); nw += int((~isr).sum()); nr += int(isr.sum())
meta = dict(generator='gen_synth_trace.py', read_pct=rd, mix_4k_pct=f4, aligned='size', seed=seed, sample='synthetic',
            kept_regions=0, capacity_bytes=L * 4096, capacity_gib=round(L * 4096 / 2**30, 2), writes=nw, reads=nr,
            write_pages=wp, read_pages=rp, write_gib=round(wp * 4096 / 2**30, 2), read_gib=round(rp * 4096 / 2**30, 2),
            turnovers=round(wp / L, 3))
json.dump(meta, open(out + '.json', 'w'), indent=1)
print(meta)
