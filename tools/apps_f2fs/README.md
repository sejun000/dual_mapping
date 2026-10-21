# Application benchmarks on the dual_mapping NVMeVirt FTLs (F2FS version, frozen)

> Frozen 2026-10-05: these are the exact scripts that produced `results/apps/` (F2FS). The suite moved to ext4 in
> `tools/apps/` (results in `results/apps_ext4/`). Note: the rebuilt NVMeVirt modules print a counter line on every
> NVMe Flush unless loaded with `dm_flush_dump=0`; these scripts take their counters with `nvme flush`, so re-running
> them needs the default `dm_flush_dump=1`.

Port of the splitEval application suite (`~/splitFTL/gyjin/_repos_staging/splitEval`, `results_128m/graphs_final`)
to our pluggable NVMeVirt FTLs. The reference configuration is the study's **conv-adaptive** baseline
(conventional FTL + F2FS `mode=adaptive`, `ipu_policy=1`), campaign `bench_mn_convadpt4k_*_20260916T*`
(`scripts/tests/mn_campaign_queue.sh`, `scripts/tests/mn_fio_queue.sh`, `scripts/run_bench.sh`,
`scripts/runner_lib.sh`, `scripts/fs_6.12.sh`, `scripts/workloads/*.sh`). Parameters below were read from those
scripts and cross-checked against each run's `experiment_config.txt` / `summary.txt` / logs.

## Files

| file | role |
|---|---|
| `run_app.sh <workload> <policy>` | one run: insmod -> mkfs.f2fs -> mount `/mnt/dmapp` -> dataset/preload -> fill -> capture -> workload -> capture -> umount -> rmmod |
| `run_all.sh` | 8 workloads x 6 policies, resumable (`--status`, `--cleanup`, `RETRY_FAILED=1`, `WORKLOADS=`, `POLICIES=`) |
| `common.sh` | chassis: device identification/refusal, module load, B18, F2FS, ballast, samplers, capture |
| `wl/{filebench,fio,ycsb,tpcc}.sh` | workload plugins (ports of splitEval `workloads/*.sh`) |
| `pg_ycsb.sh` | splitEval `ycsb_run_scripts/postgres_run.sh`, only the data root made a parameter (`diff` in its header) |
| `filebench/*.f` | the study's three conv `.f` files, byte-identical (sha256 matches the study runs) |
| `blklat.bt` | bpftrace: block-layer issue->complete latency histograms + bytes written through F2FS |
| `summarize.py` | `case <dir>` -> `summary.txt`/`summary.json`; `table [root]` -> TSV over all cases |
| `smoke.sh` | every plugin end-to-end at toy scale on a loop F2FS image in `/mnt/nvme_mounted/dmapp_smoke` (no NVMeVirt) |

Results: `results/apps/<workload>/<policy>/` with `STATUS` (`DONE` / `FAILED:<why>` / `RUNNING`), `summary.txt`,
`experiment_config.txt`, `run.log`, `dm_{policy,start,end,final}.txt` (+ `dm_measure.txt` for fio), raw workload
output, timeseries, `dmesg.txt`. `results/apps/state.log` is an append-only START/END log; `results/apps/summary.tsv`
is written when the matrix finishes.

## Running (not run yet)

```
tools/apps/smoke.sh                       # optional re-check, loop device only
tools/apps/run_all.sh --status
# queue behind the running NVMeVirt experiments (pattern with [n] so it never matches itself):
nohup bash -c 'while pgrep -f "[n]vmev_cycle.sh" >/dev/null; do sleep 60; done; tools/apps/run_all.sh' \
      > results/apps/run_all.log 2>&1 &
tools/apps/run_all.sh --cleanup           # only after a killed run; touches /mnt/dmapp + nvmev only if they are ours
```

Safety: the target is the single namespace whose `/sys/block/*/device/model` **and** `lsblk MODEL` start with
`CSL_Virt`; `nvme0n1 nvme1n1 nvme2n1` are refused by name regardless; a device with holders or mounted elsewhere is
refused. `run_app.sh`/`run_all.sh` refuse to start if `nvmev` is already loaded or an `nvmev_cycle/randwrite/mixed`
script is running, and only ever `rmmod` a module they loaded themselves. Loop mode (smoke) accepts only loop
devices backed by `/mnt/nvme_mounted/dmapp_smoke/*`. One run at a time (`flock /tmp/dmapp_bench.lock`). Nothing is ever killed by process name: teardown signals only the
PIDs the plugin started (fio, YCSB/BenchBase clients), its own systemd units (`dmapp-filebench-*`, `dmapp_pg_tpccm_*`)
and its own PostgreSQL clusters (`pg_ctl -D <dir under /mnt/dmapp>`).
The driver stops on kernel trouble in a run's dmesg (BUG/Oops/soft lockup/hung task/nvmev WARNING), like the study's queue guard.

## Common configuration (chassis)

| item | study conv-adaptive | here |
|---|---|---|
| kernel / F2FS | 6.12.49+, patched `f2fs.ko` (sha 88ae028f) | 6.8.0-142, stock F2FS |
| module | `conv/nvmev_BASELINE.ko`, `memmap_start=128G memmap_size=44G cpus=2,4,6,8 nand_read_prio=1 map_unit_kb=4` | `~/nvmevirt_<policy>/nvmev.ko memmap_start=256G memmap_size=44G cpus=94,95 dm_params=<policy>` |
| dm_params | - | as `tools/nvmev_cycle.sh`: page4k/fast `""`; proposed V3 (`U=16,...,map_budget_ppm=187500`); dftl `cache_bytes=B18,dftl_tpbuf=1`; leaftl/mu `cache_bytes=B18`. B18 = L x 4 x 0.1875 with L read from dmesg of a page4k load (computed once, cached in `results/apps/b18_44G.txt`; expected L = 10,798,160 -> B18 = 8,098,620) |
| device | ns1 2 GiB conventional (F2FS metadata) + ns2 40 GiB data, same dies | one namespace: 373 lines x 30976 pages = 45133 MiB NAND, 42180 MiB exposed (OP 7%) |
| mkfs | `mkfs.f2fs -f -s 64 -c <data> <meta>` | `mkfs.f2fs -f -s 64 <dev>` (f2fs-tools 1.16.0) |
| F2FS size | MAIN 19904 segs (OP 1464), 311 sections, df 42.15 GB | MAIN 20800 segs (OP 1503), 325 sections, df 44.10 GB (previewed on a same-size sparse loop file) |
| mount options | (none passed; findmnt shows the list) | the recorded list passed explicitly: `rw,relatime,lazytime,background_gc=on,nogc_merge,discard,discard_unit=block,user_xattr,inline_xattr,acl,inline_data,inline_dentry,flush_merge,barrier,extent_cache,mode=adaptive,active_logs=6,alloc_mode=default,checkpoint_merge,fsync_mode=posix,memory=normal,errors=continue` (stock 6.8 accepts all; findmnt reads back the identical string) |
| sysfs | `ipu_policy=1` (FORCE), `min_ssr_sections=20` | `ipu_policy=1` (stock default is 129), `min_ssr_sections=20` written explicitly |
| block queue | scheduler none, wbt 2000 us, `nvme_core.max_retries=255` | same (`io_timeout>=255` kept) |
| placement | `numactl --cpunodebind=1 --membind=1`; f2fs kthreads pinned to node 1; NVMeVirt CPUs on node 0 | `numactl --physcpubind=<node 1 minus 95> --membind=1`; f2fs kthreads pinned to the same CPUs |
| memory limits | filebench: systemd-run `MemoryMax=8G MemoryHigh=7G`; TPC-C: each PostgreSQL unit `1G/896M`; YCSB, fio: none | **one common cgroup for every workload** (user decision 2026-10-04): transient `dmapp.slice` with `MemoryMax` = 20% of the device's logical size (`blockdev --getsize64`, rounded down to MiB; study 8G on ~40 GB) and `MemoryHigh` = 7/8 of that. On the 44G device: 44,229,058,560 B -> 8436 MiB / 7381 MiB. TPC-C PostgreSQL units keep their 1G/896M limits nested inside the slice |
| CPU governor | performance | performance during the matrix, previous governor restored when `run_all.sh` exits |
| utilisation gate | abort if start util outside window | same (`UTIL_CHECK=abort`) |
| post-stop | 45 s observation, f2fs status | same |

Memory cgroup placement: `setup_memcg` (after insmod) refuses to start if `dmapp.slice` still has processes, stops the
empty slice (fresh `memory.peak`/`memory.events`) and sets the limits with `systemctl set-property --runtime`.
systemd units get `--slice=dmapp.slice` (filebench unit, TPC-C PostgreSQL units); everything started directly goes
through `$SCOPE` = `sudo systemd-run --scope --slice=dmapp.slice --uid=<user>` (fio prefill + bench, every
`pg_ycsb.sh` call so YCSB's `pg_ctl`-started postgres and its loader/client JVMs, BenchBase load/execute JVMs).
Page cache of the whole workload is therefore charged to one cap. Not in the slice: mkfs/mount, the ballast `dd`
(fill step, followed by sync + drop_caches), `initdb`/`psql` admin calls, samplers/bpftrace. Recorded per run:
`memcg_*` lines in `experiment_config.txt`, `memcg_start.txt`/`memcg_end.txt` (live `memory.max/high/current/peak`,
`memory.events`, and the pid/comm/cgroup of every member), `memcg_timeseries.txt` (2 s), and
`memcg_{max,peak,events_high,events_max,events_oom_kill}` in `summary.txt`. At the end of every run the slice is
stopped, which ends anything of ours still in it (cgroup-scoped, never by process name).

Capture (start = at measurement start, end = after the workload): `nvme flush` -> last `[dm flush]` dmesg line
(device counters), `/sys/block/<dev>/stat`, df, F2FS debugfs status (our partition only), F2FS sysfs counters;
1 s samplers of `inflight` + `stat` (write QD) and 2 s samplers of F2FS IPU/SSR/LFS/GC/util; bpftrace
`blklat.bt` for the whole window. `[dm final]` is recorded at rmmod.

## Workloads

| ours | study row | driver / exact parameters (study source) |
|---|---|---|
| `vmhot1g` | Varmail (hot 1 GB) | filebench `varmail-hot1g-50-perc-conv-fsutil-600s.f`: bigfileset 1,184,222 x 16 KiB (prealloc 80) cold + hotset 2048 x 512 KiB (1 GB); 16 threads: open+writewholefile 512k+fsync+close, open+readwholefile 1m+close; `create files; sync; drop_caches; psrun 600 600`; `FILEBENCH_BASE_SEED=4357`, `setarch -R`; fill 50%, window 48-80% (`run_bench.sh` via `mn_campaign_queue.sh`: `WORKLOAD=filebench-varmail-hot1g FILEBENCH_FILL=50 DURATION=600`, `EXPECTED_F2FS_USE_MAX=80`) |
| `oltp` | OLTP | `oltp-50-perc-conv-fsutil-600s.f`: datafiles 1850 x 10 MiB + 1 log 10 MiB; 10 dbwr (aiowrite 2k random dsync, iters 100), 200 shadows (2k random read), 1 lgwr (256k aiowrite dsync); 600 s; same chassis rules as above |
| `fileserverfs` | Fileserver | `fileserverfs-50-perc-conv-fsutil-600s.f`: 178,905 x 128 KiB (prealloc 80, dirwidth 20), 50 threads: create+writewholefile 1m+fsync, append 16k+fsync, readwholefile, delete, stat; 600 s |
| `tpcc` | TPC-C | BenchBase (`tpcc-postgres/target/benchbase-postgres`, Java 21 `-Xmx512m`), 16 PostgreSQL 16 clusters x (2 warehouses, 20 terminals) = 32 WH / 320 terminals, 600 s, READ_COMMITTED, batch 128, rate 100000, weights 45/43/4/4/4; PG: `synchronous_commit=on fsync=on wal_sync_method=open_datasync full_page_writes=on shared_buffers=256MB checkpoint_timeout=30s bgwriter 10ms/4000/10.0 max_wal 512MB min_wal 80MB max_connections=64 autovacuum 2 workers/10s`; each cluster in a systemd unit `MemoryMax=1G MemoryHigh=896M`; load -> ballast to 75% -> CHECKPOINT; window 72-80% (study start 76%) |
| `ycsba` | YCSB-A | YCSB 0.16 postgrenosql binding (`distfiles/YCSB`), 16 PostgreSQL clusters x 625,000 rows (10 M total) x 20 threads (320), 1 loader per cluster, `workloada` (zipfian, read 0.5 / update 0.5), `maxexecutiontime=600`, threaded client, JVM `-Xms64m -Xmx256m -XX:MaxMetaspaceSize=128m -XX:+UseSerialGC -XX:ActiveProcessorCount=16`; PG profile `metadata-pressure` (`shared_buffers=128MB full_page_writes=on wal_writer_delay=10ms bgwriter 10ms/10000/10.0 checkpoint_timeout=30s completion 0.9 max_wal 2GB min_wal 80MB autovacuum 8 workers/1s/0.005/0.005/cost_delay 0`, table fillfactor 70), `synchronous_commit=on fsync=on wal_sync_method=open_datasync max_connections=300`; ballast to 75% (the load alone reached 80% in the study) -> CHECKPOINT; window 72-80% |
| `ycsbf` | YCSB-F | as YCSB-A with `workloadf`. As in the study, `YCSB_CORE_OPTIONS="readproportion=0.5 updateproportion=0.5"` is appended to workloadf too, so the effective mix is 1/3 read, 1/3 update, 1/3 read-modify-write (study log: READ 1.358 M, UPDATE 1.358 M, RMW 0.679 M ops per tenant) |
| `fio_randwrite` | Write only | fio 3.36, one 30 GiB file, 16 jobs `--ioengine=psync --direct=0 --fdatasync=1 --bs=4k --rw=randwrite --thread --time_based --runtime=300 --ramp_time=10 --group_reporting`; prefill 1 MiB libaio O_DIRECT QD64 `end_fsync`; numactl only (`mn_fio_queue.sh`: `WORKLOAD=fio-seq FILL_TARGET_PERCENT=0 DURATION=300 FIO_SEQ_SIZE_MB=30720 FIO_SEQ_ENGINE=psync FIO_SEQ_FDATASYNC=1 FIO_SEQ_NUMJOBS=16 FIO_SEQ_RW=randwrite FIO_SEQ_BS=4k`) |
| `fio_mix` | Mix 50:50 | as above with `--rw=randrw --rwmixread=50` |

## Metrics (`summary.txt`)

* `throughput` (filebench ops/s, YCSB summed ops/s, TPC-C summed txn/s, fio read+write IOPS) and per-driver detail.
* Application latency: `app_{write,read}_lat_ms_avg` and `_p99` (YCSB per-op avg exact, percentiles = mean of tenants'
  HdrHistogram percentiles, as the study; TPC-C merged `LatencySuccessHistogramMs` with in-bucket interpolation,
  write = NewOrder/Payment/Delivery, read = OrderStatus/StockLevel; fio clat; filebench avg and max of the study's
  flowops: varmail fsyncfile2/readfile3, oltp dbwr-block/shadowread, fileserver fsyncfile1+fsyncappend1/readfile1 -
  filebench has no percentiles). YCSB additionally prints p50/p90/p99.9 (`hdrhistogram.percentiles`, reporting only);
  fio prints 50..99.99 and fdatasync latency.
* Device latency: `blk_{read,write}_lat_us_{mean,p50,p90,p99,p99.9,p99.99}` (bpftrace, 2 us buckets below 1 ms),
  `dev_{read,write}_lat_ms_avg` and `write_qd_max` from `/sys/block` exactly as the study's plot.py.
* FTL counters (delta of the `[dm flush]` snapshots): `dev_waf` (NAND programs / host 4 KiB pages), `dev_waf_{gc,rmw,fold,map}`,
  `dev_map_reads_per_host_page` / `_per_host_read`, `dev_erases`, `dev_map_miss`, `dev_mapping_mem_bytes_end`;
  fio also `devmeas_*` for the measured phase only (study capture includes the 30 GiB prefill).
* File system: `fs_waf` = device write bytes / bytes written through `f2fs_file_write_iter`, `total_waf`, F2FS
  IPU/SSR/LFS/GC deltas, start/end utilisation.

## Deviations from the study

1. **Stock F2FS on 6.8** instead of the patched 6.12 module. The mount option string is identical; stock default
   `ipu_policy` (129) is overwritten with 1; `min_ssr_sections` written as 20 (the study's value was its kernel default
   = reserved sections; ours would default to 21). `CONFIG_F2FS_IOSTAT` is off in the Ubuntu kernel, so the study's
   iostat-based FS WAF is replaced by the bpftrace `f2fs_file_write_iter` byte count (mmap writes are not counted).
2. **No separate metadata namespace.** The study formatted F2FS over two namespaces of the same NVMeVirt
   (2 GiB conventional ns1 for SB/CP/SIT/NAT/SSA + 40 GiB ns2); here F2FS metadata goes through the policy FTL on
   the single namespace. Device WAF is still device-wide in both cases.
3. **Capacity +4.6%.** 44G memmap gives 42180 MiB with our 7% OP (study 40 GiB + 2 GiB); F2FS has 325 sections /
   19297 user segments vs 311 / 18440. The filebench cold fileset (`$nfiles` only, hot sets untouched) is scaled by
   user capacity (`FB_SCALE=auto` = 19297/18440 = 1.0465 on this device) so the 50% fill holds; `FB_SCALE=1` reproduces
   the study's absolute dataset (~47.8% fill). PostgreSQL fills are percentage-based (same rule). fio keeps the
   absolute 30 GiB, so it starts at a lower fraction of the FS than the study (~80% vs ~83% of user blocks).
   FAST truncates L to whole blocks (namespace ~70 MiB smaller).
4. **FTL / emulator.** Study: conv FTL with a 4 KiB map unit, `nand_read_prio=1`, DSM implemented (but its
   `trim_called` counter did not move during any checked conv-adaptive measurement window). Ours: dm_ftl cores,
   no read-priority knob, **no DSM** (`discard_max_bytes=0`, so F2FS `discard` is inert), write-through (`vwc=0`, as
   the study: fsync issues no NVMe flush, so the `[dm flush]` snapshots are the only flushes).
5. **NVMeVirt CPUs** 94,95 (1 dispatcher on node 0 + 1 IO worker on node 1, per `nvmev_cycle.sh`) vs the study's
   2,4,6,8 on node 0. CPU 95 is removed from the workload CPU set to keep the busy-polling worker uncontended; the
   reserved region (256-352 GiB) appears to lie in node 1's physical range (node 0 memory ends at 256 GiB, node 1's
   online memory starts at 352 GiB), whereas the study placed device memory on node 0.
6. **Software.** PostgreSQL 16.15 (Ubuntu apt; study 16.x, minor not recorded); OpenJDK 21.0.12 for BenchBase and
   YCSB (study: Java 21 path for BenchBase; YCSB's `/usr/bin/java` version not recorded); same filebench binary,
   fio 3.36, YCSB 0.16 and BenchBase builds.
7. **Added instrumentation** (identical for all policies): bpftrace probes (`BLKLAT=0` disables), extra
   percentile reporting in YCSB/fio, an extra fio snapshot after the prefill.
8. **Kept study quirks:** YCSB-F 1/3/1/3/1/3 mix; YCSB `max_connections=300` although `MAX_CONNECTIONS=64` is set;
   fio start capture before the prefill; YCSB JVM options unchanged (`-Xms64m -Xmx256m -XX:MaxMetaspaceSize=128m`;
   PostgreSQL `shared_buffers=128MB`).
9. **Memory cgroup** differs from the study by design (see the chassis table): one 20%-of-device slice around every
   workload, where the study capped only filebench (8G/7G) and the TPC-C PostgreSQL units (1G).

## Open questions

Decided 2026-10-04: no DSM/TRIM support (kept as is); one common 20%/17.5% memory cgroup; filebench cold fileset
scaled to hold 50% fill (`FB_SCALE=auto`).

* Memory pressure at full scale (not testable without the device): the 8436 MiB cap now has to hold 16 PostgreSQL
  clusters (YCSB: 16 x 128 MB shared_buffers + 320 backends; TPC-C: up to 16 x 1G units) plus 16 JVMs (YCSB
  `-Xmx256m`, BenchBase `-Xmx512m`, both for load and run) plus page cache. Watch `memcg_events_high/max/oom_kill`
  and `memcg_peak` in the first PostgreSQL runs; OOM kills inside the slice are also written to the run's
  `warnings.txt` (a killed YCSB/BenchBase JVM fails the run; a killed PostgreSQL backend may not).
* Runtime: roughly 15-30 min per run (fills/loads at the slower policies' write speed), ~15-25 h for 48 runs.
