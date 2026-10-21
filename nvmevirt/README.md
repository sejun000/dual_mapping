# NVMeVirt FTL trees used in the DUET evaluation

Each folder is a snapshot (`git archive`) of one `~/nvmevirt_<policy>` tree, a fork of
[snu-csl/nvmevirt](https://github.com/snu-csl/nvmevirt) whose local branch `dm-<policy>` holds the changes.
All seven share the same NVMeVirt core and glue (`dm_ftl.c`); they differ only in the FTL under `dm/`.

| folder | FTL | module params used in the paper (12.5% mapping DRAM, OP 7%) |
|---|---|---|
| page4k | page-level, full map in DRAM | (none) |
| proposed | DUET (dual-granularity + spill + clean read-promotion) | see `tools/apps/common.sh`, alias `proposed_b12r` |
| dftl | DFTL | `cache_bytes=B12,dftl_tpbuf=1` |
| leaftl | LeaFTL (512 KiB sort batch, staged pages hold buffer) | `cache_bytes=B12,leaftl_batch=128` |
| fast | FAST (log blocks capped by worst-case map memory) | `cache_bytes=B12` |
| mu | mu-FTL | `cache_bytes=B12` |
| iurmw | 32 KiB IU + RMW | `U=8` |

Shared changes vs upstream: host-read priority with program/erase suspend (`ssd.c`), credit-paced background NAND ops
and staged-write buffer accounting (`dm_ftl.c`), admin Identify PRP overrun fix (`admin.c`), OP 7% geometry (`ssd_config.h`).
Build: `make` in a folder (kernel module `nvmev.ko`); load via `tools/nvmev_cycle.sh`.
