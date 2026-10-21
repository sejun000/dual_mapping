// SPDX-License-Identifier: GPL-2.0-only
/*
 * dual_mapping: NVMeVirt namespace glue for pluggable FTL cores (dm/ftl_<policy>.c), replacing conv_ftl.c.
 * Keeps conv_ftl's entry points (conv_init_namespace / conv_remove_namespace / conv_proc_nvme_io_cmd) so
 * main.c is untouched. One FTL instance over the whole LBA space (SSD_PARTITIONS must be 1).
 *
 * Geometry: a core block == an NVMeVirt line (superblock: the same block index on every ch/lun),
 * P = pgs_per_blk * nchs * luns_per_ch, NB = blks_per_pl. In-block offsets follow conv_ftl's write-pointer
 * order (page within oneshot page, then channel, then LUN, then next wordline). Logical pages
 * L = NB*P / (1 + OP_AREA_PERCENT), rounded down to the IU size.
 *
 * Timing: core flash callbacks become ssd_advance_nand() commands. Programs are issued once per oneshot
 * page (on its last 4 KiB page) like conv_ftl; host-path work (host / RMW / fold / map) is USER_IO at the
 * request's time. Background work (GC / fold copies, reclaim erases) is queued and paced (dm_bg_*, see below) instead
 * of being issued all at once, and host reads have priority over programs/erases on a LUN (ssd.c). Host reads of the same flash page
 * are aggregated. Mapping-table pages (ppn == DM_NOPPN) are not placed in data blocks; they cost a 4 KiB read
 * on a round-robin LUN; map-page programs are packed pgs_per_oneshotpg per oneshot program (small buffer). Pages still in the shared IU write buffer are served from DRAM.
 * Map / misprediction reads on the host path are serialized before the data read (dependency).
 * dm_params "U=<pages>" sets the IU (write-buffer granularity, and the policy's IU where it has one).
 * Stats are printed on NVMe flush and on module removal.
 */
#include <linux/vmalloc.h>
#include <linux/moduleparam.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "ssd.h"
#include "conv_ftl.h"
#include "dm/ftlcore.h"
#include "dm/dm_wbuf.h"

#if SSD_PARTITIONS != 1
#error "dm_ftl requires SSD_PARTITIONS == 1"
#endif
#ifndef DM_WBUF_BYTES
#define DM_WBUF_BYTES (1u << 20)
#endif
#ifndef DM_IU_PAGES
#define DM_IU_PAGES 16
#endif
#define DM_OP_PPM ((u64)(OP_AREA_PERCENT * 1000000))
/* Background NAND work (GC / fold copies, reclaim erases) is credit-paced: queued, and each host write of n pages
 * issues ~rate*n queued ops (rate = background ops per host page, measured over DM_BG_RWIN host pages, x5/4) plus
 * bg_n/1024, so a whole superblock's copies never land on the LUNs at once (the 1 MiB write buffer would block every
 * host I/O for up to ~1.6 s behind them). The FTL logic and its counters are unchanged; only NAND timing moves. */
#define DM_BG_QMAX (1u << 20)
#define DM_BG_RWIN (1ull << 16)
struct dm_bgop { struct ppa p; u32 bytes; int cmd; };
#define DM_PEND_MAX 8192
struct dm_pend { int sq; u32 bytes; };

static char *dm_params = "";
module_param(dm_params, charp, 0444);
/* print the counter line on every NVMe Flush (legacy hook used by the raw-device scripts); 0 for fsync-heavy runs */
static bool dm_flush_dump = true;
module_param(dm_flush_dump, bool, 0644);
MODULE_PARM_DESC(dm_flush_dump, "Print the [dm flush] counter line on every NVMe Flush (default 1)");
static struct dm_ns *dm_cur;   /* the (single) namespace, for the sysfs dump trigger */
MODULE_PARM_DESC(dm_params, "FTL policy options, space-separated key=value (e.g. \"cache_bytes=6M\")");

struct dm_ns {
	struct ssd *ssd;
	struct ssdparams *spp;
	void *core;
	struct dm_geom g;
	struct dm_stats st;
	struct dm_wbuf wb;
	/* request context */
	int in_host;
	u64 stime, latest;
	/* pending read aggregation */
	int rd_pending, rd_src;
	struct ppa rd_ppa;
	u32 rd_bytes;
	u32 map_rr;
	u32 map_pend;   /* map pages buffered toward the next oneshot program */
	u64 host_done;  /* latest completion time of any host-path NAND op issued so far (Flush waits for this) */
	struct dm_bgop *bg; u32 bg_head, bg_n;
	struct dm_pend *pend; u32 pend_n;   /* write-buffer space of requests whose pages sit in the core's sort batch */
	u64 bg_in, bg_win_in, bg_win_pages, bg_forced; u32 bg_rate_x1024;   /* rate = bg ops per host page, x1024 */
};

static struct ppa dm_ppn2ppa(struct dm_ns *d, u64 ppn, int *last_in_os)
{
	struct ssdparams *spp = d->spp;
	u32 os = spp->pgs_per_oneshotpg, nch = spp->nchs, nlun = spp->luns_per_ch;
	u32 per_wl = os * nch * nlun, blk = (u32)(ppn / d->g.P), o = (u32)(ppn % d->g.P);
	u32 wl = o / per_wl, r = o % per_wl;
	struct ppa p;
	p.ppa = 0;
	p.g.blk = blk;
	p.g.lun = r / (os * nch);
	p.g.ch = (r / os) % nch;
	p.g.pg = wl * os + r % os;
	p.g.pl = 0;
	if (last_in_os) *last_in_os = (r % os) == os - 1;
	return p;
}

static struct ppa dm_map_ppa(struct dm_ns *d)
{
	struct ppa p;
	p.ppa = 0;
	p.g.ch = d->map_rr % d->spp->nchs;
	p.g.lun = (d->map_rr / d->spp->nchs) % d->spp->luns_per_ch;
	d->map_rr++;
	return p;
}

static void dm_bg_issue_head(struct dm_ns *d, u64 stime)
{
	struct dm_bgop *o = &d->bg[d->bg_head];
	struct nand_cmd c = { .type = GC_IO, .cmd = o->cmd, .stime = stime, .xfer_size = o->bytes,
			      .interleave_pci_dma = false, .ppa = &o->p };
	ssd_advance_nand(d->ssd, &c);
	d->bg_head = (d->bg_head + 1) % DM_BG_QMAX; d->bg_n--;
}

/* idle fill: queued background ops whose LUN is idle at "now" go out immediately (background GC uses idle NAND time;
 * a busy LUN is left alone, so this never delays host I/O) */
static void dm_bg_fill(struct dm_ns *d, u64 now)
{
	u32 budget = 256;
	while (d->bg_n && budget-- && get_lun(d->ssd, &d->bg[d->bg_head].p)->next_lun_avail_time <= now)
		dm_bg_issue_head(d, now);
}

/* credit pacing: called once per host write of n pages, before its own flash work */
static void dm_bg_pace(struct dm_ns *d, u64 n, u64 stime)
{
	u64 m;
	d->bg_win_pages += n;
	if (d->bg_win_pages >= DM_BG_RWIN) {   /* re-measure background ops per host page */
		d->bg_rate_x1024 = (u32)min_t(u64, (d->bg_in - d->bg_win_in) * 1024 / d->bg_win_pages, 1u << 30);
		d->bg_win_in = d->bg_in; d->bg_win_pages = 0;
	}
	m = (n * d->bg_rate_x1024 * 5 / 4 + 1023) / 1024 + d->bg_n / 1024;
	while (m-- && d->bg_n)
		dm_bg_issue_head(d, stime);
}

static u64 dm_issue(struct dm_ns *d, int cmd, struct ppa *p, u32 bytes, int host_path, bool pci)
{
	if (!host_path && d->bg) {
		if (d->bg_n == DM_BG_QMAX) { dm_bg_issue_head(d, 0); d->bg_forced++; }
		d->bg[(d->bg_head + d->bg_n) % DM_BG_QMAX] = (struct dm_bgop){ .p = *p, .bytes = bytes, .cmd = cmd };
		d->bg_n++; d->bg_in++;
		return 0;
	}
	{
	struct nand_cmd c = {
		.type = host_path ? USER_IO : GC_IO,
		.cmd = cmd,
		.stime = host_path ? d->stime : 0,
		.xfer_size = bytes,
		.interleave_pci_dma = pci,
		.ppa = p,
	};
	u64 t = ssd_advance_nand(d->ssd, &c);
	if (host_path && t > d->latest)
		d->latest = t;
	if (host_path && t > d->host_done)
		d->host_done = t;
	return t;
	}
}

static void dm_read_flush(struct dm_ns *d)
{
	if (!d->rd_pending)
		return;
	dm_issue(d, NAND_READ, &d->rd_ppa, d->rd_bytes, d->in_host && (d->rd_src == DM_R_HOST || d->rd_src == DM_R_RMW),
		 d->in_host && d->rd_src == DM_R_HOST);
	d->rd_pending = 0;
}

static void dm_cb_prog(void *ctx, u64 ppn, int src)
{
	struct dm_ns *d = ctx;
	struct ppa p;
	int last = 1;
	/* host-path work: the host page itself, RMW of its IU, and synchronous map I/O; GC and fold (space
	 * reclamation) are background like conv_ftl's GC (stime 0) */
	int host_path = d->in_host && (src == DM_P_HOST || src == DM_P_RMW || src == DM_P_MAP);
	if (ppn == DM_NOPPN) {
		/* translation pages are written out of place (the policy's directory records each new location), so
		 * any 4 KiB map pages can share a oneshot page: buffer them and program once per pgs_per_oneshotpg */
		if (++d->map_pend < d->spp->pgs_per_oneshotpg)
			return;
		d->map_pend = 0;
		p = dm_map_ppa(d);
	} else
		p = dm_ppn2ppa(d, ppn, &last);
	if (!last)
		return;
	dm_issue(d, NAND_WRITE, &p, d->spp->pgsz * d->spp->pgs_per_oneshotpg, host_path, false);
}

static void dm_cb_read(void *ctx, u64 ppn, int src)
{
	struct dm_ns *d = ctx;
	struct ppa p;
	if (ppn == DM_NOPPN || src == DM_R_MAP || src == DM_R_OTHER) {
		/* translation-page / misprediction (OOB) reads: the dependent data read cannot start before them */
		u64 t;
		dm_read_flush(d);
		p = ppn == DM_NOPPN ? dm_map_ppa(d) : dm_ppn2ppa(d, ppn, NULL);
		t = dm_issue(d, NAND_READ, &p, d->spp->pgsz, d->in_host, false);
		if (d->in_host && t > d->stime)
			d->stime = t;
		return;
	}
	p = dm_ppn2ppa(d, ppn, NULL);
	if (d->rd_pending && d->rd_src == src && d->rd_ppa.h.blk_in_ssd == p.h.blk_in_ssd &&
	    d->rd_ppa.g.pg / d->spp->pgs_per_flashpg == p.g.pg / d->spp->pgs_per_flashpg) {
		d->rd_bytes += d->spp->pgsz;
		return;
	}
	dm_read_flush(d);
	d->rd_pending = 1; d->rd_src = src; d->rd_ppa = p; d->rd_bytes = d->spp->pgsz;
}

static void dm_cb_erase(void *ctx, u32 blk)
{
	struct dm_ns *d = ctx;
	u32 ch, lun;
	dm_read_flush(d);
	for (ch = 0; ch < d->spp->nchs; ch++)
		for (lun = 0; lun < d->spp->luns_per_ch; lun++) {
			struct ppa p;
			p.ppa = 0; p.g.ch = ch; p.g.lun = lun; p.g.blk = blk;
			dm_issue(d, NAND_ERASE, &p, 0, 0, false);
		}
}

static void dm_emit(void *ctx, u64 lpn, u64 n)
{
	struct dm_ns *d = ctx;
	dm_policy.write(d->core, lpn, n);
}

static void dm_dump_stats(struct dm_ns *d, const char *when)
{
	u64 tp = 0; int i;
	for (i = 0; i < DM_P_N; i++) tp += d->st.prog[i];
	NVMEV_INFO("[dm %s] policy=%s L=%llu NB=%u P=%u submitted=%llu host=%llu prog=%llu (host %llu gc %llu rmw %llu fold %llu map %llu other %llu) "
		   "WAFx1000=%llu reads(host %llu gc %llu rmw %llu fold %llu map %llu) host_read_pages=%llu erases=%llu map_miss=%llu mem=%llu\n",
		   when, dm_policy.name, d->g.L, d->g.NB, d->g.P, d->st.host_submitted, d->st.host_pages, tp,
		   d->st.prog[DM_P_HOST], d->st.prog[DM_P_GC], d->st.prog[DM_P_RMW], d->st.prog[DM_P_FOLD], d->st.prog[DM_P_MAP], d->st.prog[DM_P_OTHER],
		   d->st.host_submitted ? tp * 1000 / d->st.host_submitted : 0,
		   d->st.read[DM_R_HOST], d->st.read[DM_R_GC], d->st.read[DM_R_RMW], d->st.read[DM_R_FOLD], d->st.read[DM_R_MAP],
		   d->st.host_read_pages, d->st.erases, d->st.map_miss, dm_policy.mem_bytes(d->core));
}

void conv_init_namespace(struct nvmev_ns *ns, uint32_t id, uint64_t size, void *mapped_addr,
			 uint32_t cpu_nr_dispatcher)
{
	struct ssdparams spp;
	struct dm_ns *d = kzalloc(sizeof(*d), GFP_KERNEL);
	struct dm_flash_ops fo;
	u64 np;

	ssd_init_params(&spp, size, 1);
	d->ssd = kmalloc(sizeof(struct ssd), GFP_KERNEL);
	ssd_init(d->ssd, &spp, cpu_nr_dispatcher);
	d->spp = &d->ssd->sp;

	d->g.P = d->spp->pgs_per_blk * d->spp->nchs * d->spp->luns_per_ch * d->spp->pls_per_lun;
	d->g.NB = d->spp->blks_per_pl;
	{ u64 u = DM_IU_PAGES; dm_param_u64(dm_params, "U", &u); d->g.U = (u32)u; }
	d->g.T = 2;
	np = (u64)d->g.NB * d->g.P;
	d->g.L = np * 1000000ull / (1000000ull + DM_OP_PPM);
	d->g.L -= d->g.L % d->g.U;

	fo.ctx = d; fo.prog = dm_cb_prog; fo.read = dm_cb_read; fo.erase = dm_cb_erase;
	d->core = dm_policy.create(&d->g, &fo, &d->st, dm_params);
	NVMEV_ASSERT(d->core);
	dm_wbuf_init(&d->wb, d->g.U, DM_WBUF_BYTES / 4096, dm_emit, d, &d->st.host_submitted);
	d->bg = vzalloc(sizeof(struct dm_bgop) * DM_BG_QMAX);   /* NULL -> unpaced */
	d->pend = vzalloc(sizeof(struct dm_pend) * DM_PEND_MAX);

	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->nr_parts = 1;
	ns->ftls = (void *)d;
	ns->size = (dm_policy.logical_pages ? dm_policy.logical_pages(d->core) : d->g.L) * 4096;
	ns->mapped = mapped_addr;
	ns->proc_io_cmd = conv_proc_nvme_io_cmd;
	dm_cur = d;

	NVMEV_INFO("dm_ftl policy=%s params=\"%s\": NB=%u lines x P=%u pages, logical %llu pages (%llu MiB), physical %llu MiB\n",
		   dm_policy.name, dm_params, d->g.NB, d->g.P, d->g.L, d->g.L >> 8, np >> 8);
}

void conv_remove_namespace(struct nvmev_ns *ns)
{
	struct dm_ns *d = (struct dm_ns *)ns->ftls;
	if (!d)
		return;
	dm_cur = NULL;
	dm_wbuf_drain(&d->wb);
	dm_policy.flush(d->core);
	dm_dump_stats(d, "final");
	if (dm_policy.verify(d->core))
		NVMEV_ERROR("dm_ftl: verify FAILED at removal\n");
	dm_policy.destroy(d->core);
	dm_wbuf_destroy(&d->wb);
	NVMEV_INFO("dm_ftl: background ops %llu (forced %llu, left %u, rate %u/1024 per host page)\n", d->bg_in, d->bg_forced, d->bg_n, d->bg_rate_x1024);
	vfree(d->bg);
	vfree(d->pend);
	ssd_remove(d->ssd);
	kfree(d->ssd);
	kfree(d);
	ns->ftls = NULL;
}

static bool dm_write(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct dm_ns *d = (struct dm_ns *)ns->ftls;
	struct ssdparams *spp = d->spp;
	struct buffer *wbuf = d->ssd->write_buffer;
	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba, nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg, end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg;
	uint64_t xfer_done;
	uint32_t allocated;
	u64 st0 = d->st.staged, fl0 = d->st.stage_flushes;

	if (end_lpn >= ns->size / 4096) {
		NVMEV_ERROR("%s: lpn %lld beyond L=%lld\n", __func__, end_lpn, ns->size / 4096);
		return false;
	}
	allocated = buffer_allocate(wbuf, LBA_TO_BYTE(nr_lba));
	if (allocated < LBA_TO_BYTE(nr_lba))
		return false;
	xfer_done = ssd_advance_write_buffer(d->ssd, req->nsecs_start, LBA_TO_BYTE(nr_lba));

	if (d->bg) {
		dm_bg_fill(d, req->nsecs_start);
		dm_bg_pace(d, end_lpn - start_lpn + 1, xfer_done);
	}
	d->in_host = 1; d->stime = xfer_done; d->latest = xfer_done;
	dm_wbuf_write(&d->wb, start_lpn, end_lpn - start_lpn + 1);
	dm_read_flush(d);
	d->in_host = 0;

	/* NVMeVirt's buffer is a timing model here: release this request's bytes when its flash work is done. Pages a core
	 * stages outside the write buffer (LeaFTL's sort batch) keep their buffer space until the batch is programmed. */
	if (d->pend && d->st.stage_flushes != fl0) {
		u32 i;
		for (i = 0; i < d->pend_n; i++)
			schedule_internal_operation(d->pend[i].sq, d->latest, wbuf, d->pend[i].bytes);
		d->pend_n = 0;
		schedule_internal_operation(req->sq_id, d->latest, wbuf, allocated);
	} else if (d->pend && d->st.staged > st0 && d->pend_n < DM_PEND_MAX) {
		d->pend[d->pend_n].sq = req->sq_id; d->pend[d->pend_n].bytes = allocated; d->pend_n++;
	} else
		schedule_internal_operation(req->sq_id, d->latest, wbuf, allocated);

	ret->nsecs_target = ((cmd->rw.control & NVME_RW_FUA) || spp->write_early_completion == 0) ? d->latest : xfer_done;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

static bool dm_read(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct dm_ns *d = (struct dm_ns *)ns->ftls;
	struct ssdparams *spp = d->spp;
	struct nvme_command *cmd = req->cmd;
	uint64_t lba = cmd->rw.slba, nr_lba = (cmd->rw.length + 1);
	uint64_t start_lpn = lba / spp->secs_per_pg, end_lpn = (lba + nr_lba - 1) / spp->secs_per_pg, lpn;

	if (end_lpn >= ns->size / 4096) {
		NVMEV_ERROR("%s: lpn %lld beyond L=%lld\n", __func__, end_lpn, ns->size / 4096);
		return false;
	}
	if (d->bg)
		dm_bg_fill(d, req->nsecs_start);
	d->in_host = 1;
	d->stime = req->nsecs_start + (LBA_TO_BYTE(nr_lba) <= KB(4) ? spp->fw_4kb_rd_lat : spp->fw_rd_lat);
	d->latest = d->stime;
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		d->st.host_read_pages++;
		if (!dm_wbuf_contains(&d->wb, lpn))
			dm_policy.read(d->core, lpn, 1);
	}
	dm_read_flush(d);
	d->in_host = 0;
	ret->nsecs_target = d->latest;
	ret->status = NVME_SC_SUCCESS;
	return true;
}

/* NVMe Flush: the device write buffer is power-loss protected (enterprise SSD), so a flush only has to wait for
 * host-path NAND operations already issued, not for background GC/fold work queued on the LUNs. */
static void dm_flush_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct dm_ns *d = (struct dm_ns *)ns->ftls;
	if (dm_flush_dump)
		dm_dump_stats(d, "flush");
	ret->status = NVME_SC_SUCCESS;
	ret->nsecs_target = max(local_clock(), d->host_done);
}

/* echo 1 > /sys/module/nvmev/parameters/dm_dump prints the counter line (tagged "flush" for script compatibility) */
static int dm_dump_set(const char *val, const struct kernel_param *kp)
{
	if (dm_cur)
		dm_dump_stats(dm_cur, "flush");
	return 0;
}
static const struct kernel_param_ops dm_dump_ops = { .set = dm_dump_set, .get = param_get_int };
static int dm_dump_dummy;
module_param_cb(dm_dump, &dm_dump_ops, &dm_dump_dummy, 0200);

bool conv_proc_nvme_io_cmd(struct nvmev_ns *ns, struct nvmev_request *req, struct nvmev_result *ret)
{
	struct nvme_command *cmd = req->cmd;

	NVMEV_ASSERT(ns->csi == NVME_CSI_NVM);
	switch (cmd->common.opcode) {
	case nvme_cmd_write:
		return dm_write(ns, req, ret);
	case nvme_cmd_read:
		return dm_read(ns, req, ret);
	case nvme_cmd_flush:
		dm_flush_cmd(ns, req, ret);
		break;
	default:
		NVMEV_ERROR("%s: command not implemented: %s (0x%x)\n", __func__,
			    nvme_opcode_string(cmd->common.opcode), cmd->common.opcode);
		break;
	}
	return true;
}
