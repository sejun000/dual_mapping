#!/usr/bin/env python3
"""Mapping-DRAM budget of every bounded FTL for one NVMeVirt geometry (2026-11-11), as derived in results/nvmevirt/q_vbm.sh:
the total T = DRAM of a 32 KB-IU FTL (IU+RMW, U=8) = its map L*4/8 + one valid bit per physical 32 KB IU (NB*P/64 B), and
each FTL pays its own GC valid information and directory out of T:
  DFTL   cache_bytes = T - NB*P/8 (bit per page) - floor(L/1024)*4 (GTD, dftl_mem)
  LeaFTL cache_bytes = T - NB*P/8 - ceil(L/256)*4 (frame GTD, leaftl_mem)
  FAST   cache_bytes = T (log map caps the random log blocks; data pages need no valid bits)
  CoMap  cache_bytes = T - (NB - NLB - Tgc)*P/8 (bit per page of its non-data blocks; its GTD is inside cache_bytes)
  DUET   map_budget_ppm = (T - NB*P/16/8 (bit per 64 KB chunk) - fq*P/8 (bit per page of the fine blocks)) / (L*4)
         fine quota fq = uniform-mode cap NB - ceil(L/P) - Tgc - 3 - 1 (rule of proposed_b12q: 18 at 44G)
  python3 tools/budget_vb.py <memmap> [NB]    (NB default from NVMEV_BLKS or 373)"""
import sys, os, math
sys.argv += [] ; mm = sys.argv[1]
if len(sys.argv) > 2: os.environ['NVMEV_BLKS'] = sys.argv[2]
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nvmev_geom as ng
g = ng.geom(ng.parse(mm)); L, P, NB, TGC = g['L'], g['P'], g['NB'], 2
NP = NB * P; NLB = (L - L % P) // P
T = L * 4 // 8 + NP // 64
dftl = T - NP // 8 - (L // 1024) * 4
leaftl = T - NP // 8 - (-(-L // 256)) * 4
comap = T - (NB - NLB - TGC) * P // 8
fq = NB - (-(-L // P)) - TGC - 3 - 1
duet_bytes = T - NP // 16 // 8 - fq * P // 8
ppm = duet_bytes * 1000000 // (L * 4)
fq_ppm = next(p for p in range(0, 1000000, 100) if NB * p // 1000000 == fq)   # smallest ppm (step 100) giving fq blocks
print(f'geometry memmap={mm} NB={NB} P={P} pages ({P*4096>>20} MiB superblock, {P*4096//16>>20} MiB block/die) '
      f'L={L} ({L*4096/2**30:.3f} GiB) L_whole={L - L % P} NLB={NLB}')
print(f'T={T}  ({T/(L*4)*100:.2f}% of the page map)')
print(f'dftl cache_bytes={dftl}\nleaftl cache_bytes={leaftl}\nfast cache_bytes={T}\ncomap cache_bytes={comap}')
print(f'duet fq={fq} fine_quota_ppm={fq_ppm} map_budget_ppm={ppm} (map bytes {duet_bytes})')
