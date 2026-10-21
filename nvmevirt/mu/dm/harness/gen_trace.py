#!/usr/bin/env python3
"""Synthetic validation traces (CSV dev,op,byte_off,len) for harness <-> ftlsim parity checks.
  gen_trace.py skew  out.csv   8 GiB, zipf 0.99 over 64K-IU ranks, 2^k-page requests k=0..6, 4x capacity, 10% reads
  gen_trace.py rand4k out.csv  8 GiB, uniform 4K random, 3x capacity, 10% reads"""
import sys, numpy as np
kind, out = sys.argv[1], sys.argv[2]
rng = np.random.default_rng(7)
L = (8 << 30) // 4096; U = 16; NIU = L // U
if kind == 'skew':
    w = 1.0 / np.arange(1, NIU + 1) ** 0.99; cdf = np.cumsum(w) / w.sum()
    n = int(4 * L / 18.1)
    iu = np.searchsorted(cdf, rng.random(n)); k = rng.integers(0, 7, n); sz = 1 << k
    start = iu * U + np.where(sz < U, rng.integers(0, U, n) // sz * sz, 0)
    start = np.minimum(start, L - sz)
elif kind == 'rand4k':
    n = 3 * L; start = rng.integers(0, L, n); sz = np.ones(n, dtype=np.int64)
else:
    sys.exit('kind: skew | rand4k')
rd = rng.random(n) < 0.10
with open(out, 'w') as f:
    for s, z, r in zip(start.tolist(), sz.tolist(), rd.tolist()):
        f.write(f"0,{'R' if r else 'W'},{s * 4096},{z * 4096}\n")
print(out, n, 'requests')
