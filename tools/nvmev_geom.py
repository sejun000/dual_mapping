#!/usr/bin/env python3
"""NVMeVirt dm_ftl geometry (ssd_config.h SAMSUNG_970PRO profile of ~/nvmevirt_*: 373 blocks/plane, 8 ch x 2 LUN x 1 plane,
32 KiB flash pages, OP 7%, U=16) as a function of memmap_size, mirroring ssd_init_params() and conv_init_namespace().

  nvmev_geom.py <memmap>            -> prints L, P, FAST's logical pages, device bytes
  nvmev_geom.py --fit <cap_bytes>   -> smallest memmap_size (in MiB, printed as e.g. 20480M) whose logical space holds
                                       cap_bytes for every policy (FAST exposes L - L % P only); refuses above 96G
"""
import sys
MiB = 1 << 20
BLKS, DIES, ONESHOT, OP_PPM, U = 373, 16, 32768, 70000, 16

def geom(memmap_bytes):
    storage = memmap_bytes - MiB                       # main.c: storage starts at +1 MiB
    blk = -(-storage // (BLKS * DIES))                  # DIV_ROUND_UP(capacity, blks*pls*luns*chs)
    oneshot = -(-blk // ONESHOT)
    P = oneshot * (ONESHOT // 4096) * DIES              # pages per line (superblock)
    np_ = BLKS * P
    L = np_ * 1000000 // (1000000 + OP_PPM); L -= L % U
    return dict(L=L, P=P, NB=BLKS, L_fast=L - L % P, bytes=L * 4096, bytes_fast=(L - L % P) * 4096)

def parse(s):
    s = s.strip(); m = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}
    return int(s[:-1]) * m[s[-1].upper()] if s[-1].upper() in m else int(s)

if __name__ == '__main__':
    if sys.argv[1] == '--fit':
        cap = int(sys.argv[2])
        for mib in range(64, 96 * 1024 + 1):
            if geom(mib * MiB)['bytes_fast'] >= cap:
                print(f"{mib}M"); sys.exit(0)
        sys.exit(f"capacity {cap} does not fit in memmap_size<=96G")
    print(geom(parse(sys.argv[1])))
