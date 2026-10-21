#!/usr/bin/env python3
"""Part C1: mapping DRAM vs capacity (binary TiB; L = capacity / 4 KiB). Per-entry sizes from ftlsim's RAM model
(report(): mem) and the NVMeVirt ports' mem_bytes():
  page4k  : L x 4 B (32-bit PPN; > 16 TiB of physical pages needs 33+ bit PPNs -> 5 B entries, shown in [])
  proposed: coarse map NIU x 4 B (U-aligned extent start) + cuckoo2 table ct_nb x 4 ways x 8 B (lpn 4 B + ppn 4 B,
            rec_block=1: no per-entry time) sized so that coarse + table = budget. The NVMeVirt port additionally keeps a
            4 B/IU fragment bitmap (fbitmap[NIU], resident iff != 0) that ftlsim's model does not count; U bits/IU suffice
            (2 B at U=16) and a single 'has fragments' bit fits in the U-aligned coarse entry's low bits (0 extra).
  DFTL    : CMT (cache_bytes; entries = cache_bytes x 0.7 / 8 B) + GTD L/1024 x 4 B
  LeaFTL  : resident frames <= cache_bytes + GTD L/256 x 4 B (full learned table is workload dependent: see note)
  uFTL    : cache_bytes + 32 KiB bitmap cache + 1 B/block
  IU+RMW  : NIU x 4 B = L x 4 / U
"""
TiB = 1 << 40
def fmt(b):
    return f'{b / (1 << 30):8.2f} GiB' if b >= (1 << 30) else f'{b / (1 << 20):8.1f} MiB'
rows = []
hdr = f'{"capacity":>9s} | {"page4k":>12s} | {"prop 18.75%":>12s} | {"  coarse":>12s} | {"  cuckoo":>12s} | {"+bitmap 4B/IU":>13s} | {"DFTL 18.75%":>12s} | {"LeaFTL 18.75%":>13s} | {"IU+RMW U16":>12s} | {"IU+RMW U32":>12s}'
print(hdr); print('-' * len(hdr))
for t in (1, 4, 16, 64):
    L = t * TiB // 4096
    pm = L * 4
    big = t * 1.07 > 16          # physical pages >= 2^32 -> 5 B PPN
    pm5 = L * 5
    U = 16
    coarse = L // U * 4
    budget = pm * 1875 // 10000
    table = budget - coarse
    bitmap = L // U * 4
    dftl = budget + L // 1024 * 4
    lea = budget + L // 256 * 4
    iu16 = L // 16 * 4; iu32 = L // 32 * 4
    pg = fmt(pm) + (f' [{fmt(pm5).strip()}]' if big else '')
    print(f'{t:>5d} TiB | {pg:>12s} | {fmt(budget):>12s} | {fmt(coarse):>12s} | {fmt(table):>12s} | {fmt(bitmap):>13s} | {fmt(dftl):>12s} | {fmt(lea):>13s} | {fmt(iu16):>12s} | {fmt(iu32):>12s}')
print()
print('Per logical TiB at other operating points (fraction of the 4 B page map):')
for U in (8, 16, 32):
    for b in (0.0625, 0.10, 0.125, 0.1875, 0.25, 0.375):
        c = 4 / U / 4
        if b - c < 0.03:
            continue
        ent = (b - c) * 4 / 8           # cuckoo entries per logical page
        print(f'  U={U:2d} budget {b * 100:5.2f}%: coarse {c * 100:5.2f}% + table {(b - c) * 100:5.2f}% = {b * 1024:6.1f} MiB/TiB; '
              f'table holds {ent * 100:5.2f}% of L as 4 KiB entries (fine pool at fine_quota 0.0429: {0.0429 * 1.07 * 100:4.2f}% of L)')
