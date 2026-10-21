# Application benchmarks on the dual_mapping NVMeVirt FTLs (ext4)

**File system: ext4 since 2026-10-05 (user decision).** The first matrix ran on stock F2FS (`mode=adaptive`,
`ipu_policy=1`, the study's mount options); its results stay in `results/apps/` and the exact scripts that produced
them are frozen in `tools/apps_f2fs/` (README there). Rationale: ext4 updates file data in place, so each FTL sees
the application's own overwrite pattern rather than one already reshaped by a host-side log-structured layer
(F2FS's IPU/SSR/segment cleaning, which interacts with device GC); stock 6.8 F2FS was not the study's patched module
anyway, and ext4 is the conventional baseline file system for FTL comparisons. Everything except the file-system layer (workload parameters, 20% memory slice,
NUMA/CPU placement, capture) is unchanged. ext4-specific choices:

* `mkfs.ext4 -F -E nodiscard,lazy_itable_init=0,lazy_journal_init=0` (no TRIM - our FTLs have none - and no
  ext4lazyinit background I/O; mke2fs.conf defaults otherwise: 4 KiB blocks, 256 B inodes, 256 MiB journal,
  5% root reserve). Mount `noatime,data=ordered` (no `discard`).
* Fill levels are df-based: utilisation = df Use% = used / (used + avail), i.e. relative to the space a non-root
  user can fill (the 5% root reserve excluded). Targets = the study's start levels: filebench 50%, PostgreSQL 76%,
  fio 72%.
* FS-level WAF = block-layer write bytes / bytes copied into the page cache through ext4
  (`ext4_da_write_begin` + `ext4_write_begin` tracepoints).
* NVMeVirt modules are loaded with `dm_flush_dump=0` (no counter print per NVMe Flush, which would flood dmesg in
  fsync-heavy workloads whenever flushes reach the device); counters are taken with the sysfs trigger
  `echo 1 > /sys/module/nvmev/parameters/dm_dump`, which prints the same `[dm flush] ...` line. The rebuilt glue's
  NVMe Flush completes when the host-path NAND ops already issued are done (PLP write-buffer semantics), not after
  background GC.

Port of the splitEval application suite (`~/splitFTL/gyjin/_repos_staging/splitEval`, `results_128m/graphs_final`)
to our pluggable NVMeVirt FTLs. The reference configuration is the study's **conv-adaptive** baseline
(conventional FTL + F2FS `mode=adaptive`, `ipu_policy=1`), campaign `bench_mn_convadpt4k_*_20260916T*`
(`scripts/tests/mn_campaign_queue.sh`, `scripts/tests/mn_fio_queue.sh`, `scripts/run_bench.sh`,
`scripts/runner_lib.sh`, `scripts/fs_6.12.sh`, `scripts/workloads/*.sh`). Parameters below were read from those
scripts and cross-checked against each run's `experiment_config.txt` / `summary.txt` / logs.

## Files

| file | role |
|---|---|
| `run_app.sh <workload> <policy>` | one run: insmod (`dm_flush_dump=0`) -> mkfs.ext4 -> mount `/mnt/dmapp` -> dataset/preload -> fill -> capture -> workload -> capture -> umount -> rmmod |
| `run_all.sh` | 8 workloads x 6 policies, resumable (`--status`, `--cleanup`, `RETRY_FAILED=1`, `WORKLOADS=`, `POLICIES=`) |
| `common.sh` | chassis: device identification/refusal, module load, B18, ext4, df-based ballast, memory slice, samplers, capture |
| `wl/{filebench,fio,ycsb,tpcc}.sh` | workload plugins (ports of splitEval `workloads/*.sh`) |
| `pg_ycsb.sh` | splitEval `ycsb_run_scripts/postgres_run.sh`, only the data root made a parameter (`diff` in its header) |
| `filebench/*.f` | the study's three conv `.f` files, byte-identical (sha256 matches the study runs) |
| `blklat.bt` | bpftrace (tracepoints only): block-layer issue->complete latency histograms + bytes written through ext4 |
| `fb_nfiles.py` | filebench cold-fileset entry count that fills ext4 to the target df Use% |
| `summarize.py` | `case <dir>` -> `summary.txt`/`summary.json`; `table [root]` -> TSV over all cases |
| `smoke.sh` | every plugin end-to-end at toy scale on a loop ext4 image in `/mnt/nvme_mounted/dmapp_smoke` (no NVMeVirt) |
| `blkcap.sh` | opt-in (`CAPTURE_BLKTRACE=1`, `BLKTRACE_DIR=`): blktrace of the target device from just before mkfs to the end of the measurement (D/issue events, reads+writes) with message markers `dm_blkcap_start`, `dm_measure_start`, `dm_measure_prefill_done` (fio), `dm_measure_end`; sourced by `run_app.sh` only when the flag is set. Default dir `/mnt/nvme_mounted/dm_apptrace/<workload>` |
| `blk2csv.c` / `blk2csv` | `blk2csv <trace_dir>` -> `trace.csv` (`0,W|R,byte_off,len,ts`, issue order) + `trace.json` (totals, 4K pages as ftlsim counts them, warm-up = before `dm_measure_start`, per-marker CSV line numbers, footprints, size histograms, drops). Build: `gcc -O2 -o blk2csv blk2csv.c` |

Results: `results/apps_ext4/<workload>/<policy>/` with `STATUS` (`DONE` / `FAILED:<why>` / `RUNNING`), `summary.txt`,
`experiment_config.txt`, `run.log`, `dm_{policy,start,end,final}.txt` (+ `dm_measure.txt` for fio), raw workload
output, timeseries, `dmesg.txt`. `results/apps_ext4/state.log` is an append-only START/END log; `results/apps_ext4/summary.tsv`
is written when the matrix finishes.

## Running

```
tools/apps/smoke.sh                       # loop device only
tools/apps/run_all.sh --status
bash results/apps_ext4/run_cmd.sh         # the full matrix (started by results/nvmevirt/queue2.sh once READY exists)
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
| kernel / FS | 6.12.49+, patched `f2fs.ko` (sha 88ae028f) | 6.8.0-142, **ext4** |
| module | `conv/nvmev_BASELINE.ko`, `memmap_start=128G memmap_size=44G cpus=2,4,6,8 nand_read_prio=1 map_unit_kb=4` | `~/nvmevirt_<policy>/nvmev.ko memmap_start=256G memmap_size=44G cpus=94,95 dm_flush_dump=0 dm_params=<policy>` |
| dm_params | - | as `tools/nvmev_cycle.sh`: page4k/fast `""`; proposed V3 (`U=16,...,map_budget_ppm=187500`); dftl `cache_bytes=B18,dftl_tpbuf=1`; leaftl/mu `cache_bytes=B18`. B18 = L x 4 x 0.1875 with L read from dmesg of a page4k load (computed once, cached in `results/apps/b18_44G.txt`; expected L = 10,798,160 -> B18 = 8,098,620) |
| device | ns1 2 GiB conventional (F2FS metadata) + ns2 40 GiB data, same dies | one namespace: 373 lines x 30976 pages = 45133 MiB NAND, 42180 MiB exposed (OP 7%) |
| mkfs | `mkfs.f2fs -f -s 64 -c <data> <meta>` | `mkfs.ext4 -F -E nodiscard,lazy_itable_init=0,lazy_journal_init=0 <dev>` (e2fsprogs 1.47.0): 10,798,110 x 4 KiB blocks, 2,703,360 inodes, 64 Ki-block journal, 5% reserve |
| FS size | F2FS MAIN 311 sections, df 42.15 GB | df size 43.22 GB, used+avail 40.99 GB (previewed on a same-size sparse loop file) |
| mount options | F2FS defaults incl. `mode=adaptive,discard` | `noatime,data=ordered` (no discard) |
| FS knobs | `ipu_policy=1`, `min_ssr_sections=20` | none; the jbd2 thread is pinned to the workload CPUs (as the study pinned its F2FS kthreads) |
| block queue | scheduler none, wbt 2000 us, `nvme_core.max_retries=255` | same (`io_timeout>=255` kept) |
| placement | `numactl --cpunodebind=1 --membind=1`; f2fs kthreads pinned to node 1; NVMeVirt CPUs on node 0 | `numactl --physcpubind=<node 1 minus 95> --membind=1`; jbd2 kthread pinned to the same CPUs |
| memory limits | filebench: systemd-run `MemoryMax=8G MemoryHigh=7G`; TPC-C: each PostgreSQL unit `1G/896M`; YCSB, fio: none | **one common cgroup for every workload** (user decision 2026-10-04): transient `dmapp.slice` with `MemoryMax` = 20% of the device's logical size (`blockdev --getsize64`, rounded down to MiB; study 8G on ~40 GB) and `MemoryHigh` = 7/8 of that. On the 44G device: 44,229,058,560 B -> 8436 MiB / 7381 MiB. TPC-C PostgreSQL units keep their 1G/896M limits nested inside the slice |
| CPU governor | performance | performance during the matrix, previous governor restored when `run_all.sh` exits |
| fill / utilisation gate | F2FS Utilization; filebench 50% via fileset size, PostgreSQL ballast to 75% (+top-up), fio none; abort outside window | df Use%; filebench 50% (`fb_nfiles.py`), PostgreSQL ballast to 76% (start level the study measured), fio 72% with the 30 GiB file counted (ballast only if below); windows filebench 47-55, PostgreSQL 73-85, fio 69-82 (checked after its prefill); abort outside (`UTIL_CHECK=abort`) |
| post-stop | 45 s observation, f2fs status | 45 s, then ext4 write counters + block stat |

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

Capture (start = at measurement start, end = after the workload): `dm_dump` sysfs trigger -> new `[dm flush]` dmesg
line (device counters), `/sys/block/<dev>/stat`, df, `/sys/fs/ext4/<dev>/*_write_kbytes`, jbd2 info;
1 s samplers of `inflight` + `stat` (write QD), 2 s samplers of ext4 session writes / delalloc / df and of the memory
slice; bpftrace `blklat.bt` for the whole window (tracepoints only - see below). `[dm final]` is recorded at rmmod.

bpftrace must stay tracepoint-only: detaching a kprobe/kretprobe/fentry program waits for an RCU-tasks grace period,
which never completes while NVMeVirt's busy-polling kthreads run, so bpftrace hangs in D state until the module is
unloaded (seen once during development, 2026-10-05). `stop_samplers` gives bpftrace 60 s and then continues.

## Workloads

| ours | study row | driver / exact parameters (study source) |
|---|---|---|
| `vmhot1g` | Varmail (hot 1 GB) | filebench `varmail-hot1g-50-perc-conv-fsutil-600s.f`: bigfileset 1,184,222 x 16 KiB (prealloc 80) cold + hotset 2048 x 512 KiB (1 GB); 16 threads: open+writewholefile 512k+fsync+close, open+readwholefile 1m+close; `create files; sync; drop_caches; psrun 600 600`; `FILEBENCH_BASE_SEED=4357`, `setarch -R`; study fill 50% (`run_bench.sh` via `mn_campaign_queue.sh`: `WORKLOAD=filebench-varmail-hot1g FILEBENCH_FILL=50 DURATION=600`); here the cold-set count is recomputed for 50% df on ext4 (about 1.48 M entries on the 44G device; study 1,184,222 on F2FS, whose per-file node blocks counted towards its 50%) |
| `oltp` | OLTP | `oltp-50-perc-conv-fsutil-600s.f`: datafiles 1850 x 10 MiB (ext4 50%: about 1953) + 1 log 10 MiB; 10 dbwr (aiowrite 2k random dsync, iters 100), 200 shadows (2k random read), 1 lgwr (256k aiowrite dsync); 600 s; same chassis rules as above |
| `fileserverfs` | Fileserver | `fileserverfs-50-perc-conv-fsutil-600s.f`: 178,905 (ext4 50%: about 195,440) x 128 KiB (prealloc 80, dirwidth 20), 50 threads: create+writewholefile 1m+fsync, append 16k+fsync, readwholefile, delete, stat; 600 s |
| `tpcc` | TPC-C | BenchBase (`tpcc-postgres/target/benchbase-postgres`, Java 21 `-Xmx512m`), 16 PostgreSQL 16 clusters x (2 warehouses, 20 terminals) = 32 WH / 320 terminals, 600 s, READ_COMMITTED, batch 128, rate 100000, weights 45/43/4/4/4; PG: `synchronous_commit=on fsync=on wal_sync_method=open_datasync full_page_writes=on shared_buffers=256MB checkpoint_timeout=30s bgwriter 10ms/4000/10.0 max_wal 512MB min_wal 80MB max_connections=64 autovacuum 2 workers/10s`; each cluster in a systemd unit `MemoryMax=1G MemoryHigh=896M`; load -> ballast to 76% df -> CHECKPOINT; window 73-85% (study: ballast 75% of F2FS, start 76%) |
| `ycsba` | YCSB-A | YCSB 0.16 postgrenosql binding (`distfiles/YCSB`), 16 PostgreSQL clusters x 625,000 rows (10 M total) x 20 threads (320), 1 loader per cluster, `workloada` (zipfian, read 0.5 / update 0.5), `maxexecutiontime=600`, threaded client, JVM `-Xms64m -Xmx256m -XX:MaxMetaspaceSize=128m -XX:+UseSerialGC -XX:ActiveProcessorCount=16`; PG profile `metadata-pressure` (`shared_buffers=128MB full_page_writes=on wal_writer_delay=10ms bgwriter 10ms/10000/10.0 checkpoint_timeout=30s completion 0.9 max_wal 2GB min_wal 80MB autovacuum 8 workers/1s/0.005/0.005/cost_delay 0`, table fillfactor 70), `synchronous_commit=on fsync=on wal_sync_method=open_datasync max_connections=300`; ballast to 76% df (in the study the load alone reached 78-80%) -> CHECKPOINT; window 73-85% |
| `ycsbf` | YCSB-F | as YCSB-A with `workloadf`. As in the study, `YCSB_CORE_OPTIONS="readproportion=0.5 updateproportion=0.5"` is appended to workloadf too, so the effective mix is 1/3 read, 1/3 update, 1/3 read-modify-write (study log: READ 1.358 M, UPDATE 1.358 M, RMW 0.679 M ops per tenant) |
| `fio_randwrite` | Write only | fio 3.36, one 30 GiB file, 16 jobs `--ioengine=psync --direct=0 --fdatasync=1 --bs=4k --rw=randwrite --thread --time_based --runtime=300 --ramp_time=10 --group_reporting`; prefill 1 MiB libaio O_DIRECT QD64 `end_fsync`; fill 72% df counting the file (no ballast on the 44G device: the 30 GiB file alone is about 78.6% of used+avail, 72.8% of the device); study: numactl only (`mn_fio_queue.sh`: `WORKLOAD=fio-seq FILL_TARGET_PERCENT=0 DURATION=300 FIO_SEQ_SIZE_MB=30720 FIO_SEQ_ENGINE=psync FIO_SEQ_FDATASYNC=1 FIO_SEQ_NUMJOBS=16 FIO_SEQ_RW=randwrite FIO_SEQ_BS=4k`) |
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
* File system: `fs_waf` = block write bytes / bytes copied into ext4's page cache (fio: measured phase only, after
  the O_DIRECT prefill, and `total_waf` uses `devmeas_waf` there), `ext4_write_kbytes_delta`, start/end (fio: after
  prefill, `measure_util_pct`) df utilisation.

## Deviations from the study

1. **ext4 instead of the study's patched F2FS** (see the top of this file). No IPU/SSR/F2FS-cleaning layer; ext4
   writes data in place and journals metadata (data=ordered, 256 MiB journal). The device still advertises no
   volatile write cache (`vwc=0`), so fsync's flush requests are completed by the block layer, as in the study.
2. **No separate metadata namespace** (the study put F2FS metadata on a 2 GiB conventional namespace); everything
   goes through the policy FTL on one namespace.
3. **Capacity / fill.** 44G memmap gives 42180 MiB with our 7% OP (study 40 GiB + 2 GiB). Fill targets are df-based
   (above); the filebench cold sets are re-sized for 50% df, the PostgreSQL ballast targets 76%, and fio keeps the
   absolute 30 GiB (78.6% df, 72.8% of the device; the study's "72%" matches the device-relative number). FAST
   truncates L to whole blocks (namespace ~70 MiB smaller).
4. **FTL / emulator.** Study: conv FTL with a 4 KiB map unit, `nand_read_prio=1`, DSM implemented (but its
   `trim_called` counter did not move during any checked conv-adaptive measurement window). Ours: dm_ftl cores,
   no read-priority knob, **no DSM** (`discard_max_bytes=0`; ext4 formatted `nodiscard`, mounted without `discard`).
   Counter snapshots use the `dm_dump` sysfs trigger with `dm_flush_dump=0`, independent of NVMe Flush traffic.
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
sized to hold 50% fill (`FB_SCALE=auto`). Decided 2026-10-05: ext4 (fills 50/76/72% df), modules with
`dm_flush_dump=0`, counters via `dm_dump`, results in `results/apps_ext4/`.

* Memory pressure at full scale (not testable without the device): the 8436 MiB cap now has to hold 16 PostgreSQL
  clusters (YCSB: 16 x 128 MB shared_buffers + 320 backends; TPC-C: up to 16 x 1G units) plus 16 JVMs (YCSB
  `-Xmx256m`, BenchBase `-Xmx512m`, both for load and run) plus page cache. Watch `memcg_events_high/max/oom_kill`
  and `memcg_peak` in the first PostgreSQL runs; OOM kills inside the slice are also written to the run's
  `warnings.txt` (a killed YCSB/BenchBase JVM fails the run; a killed PostgreSQL backend may not).
* Runtime: roughly 15-30 min per run (fills/loads at the slower policies' write speed), ~15-25 h for 48 runs.
