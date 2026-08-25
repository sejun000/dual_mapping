#!/usr/bin/env python3
"""Draw the paper figures, one file per figure (PDF + PNG), Times-style serif fonts.

Usage:  python3 tools/plot.py [outdir]        (default outdir: results/fig)

Matplotlib lives in ~/tools/pyenv (extracted Debian packages); this script re-execs
itself with the right PYTHONPATH/LD_LIBRARY_PATH if matplotlib is not importable.
"""
import os
import sys

PYENV = os.path.expanduser("~/tools/pyenv")


def bootstrap():
    """Re-exec with the local matplotlib environment if it is not already importable."""
    if os.environ.get("FTLSIM_PLOT_BOOTSTRAPPED"):
        return
    try:
        import matplotlib  # noqa: F401
        return
    except ImportError:
        pass
    if not os.path.isdir(PYENV):
        sys.exit(f"matplotlib not available and {PYENV} is missing")
    libs = ":".join([
        f"{PYENV}/usr/lib/x86_64-linux-gnu",
        f"{PYENV}/usr/lib/x86_64-linux-gnu/blas",
        f"{PYENV}/usr/lib/x86_64-linux-gnu/lapack",
    ])
    env = dict(os.environ)
    env["FTLSIM_PLOT_BOOTSTRAPPED"] = "1"
    env["PYTHONPATH"] = f"{PYENV}/usr/lib/python3/dist-packages"
    env["LD_LIBRARY_PATH"] = libs + ":" + env.get("LD_LIBRARY_PATH", "")
    env["MATPLOTLIBRC"] = f"{PYENV}/usr/share/matplotlib/mpl-data"
    env.setdefault("MPLCONFIGDIR", "/tmp/ftlsim-mplcfg")
    os.execve(sys.executable, [sys.executable] + sys.argv, env)


bootstrap()

import json  # noqa: E402
import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
from matplotlib.ticker import FixedLocator, FuncFormatter  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "results", "fig")
CANON = os.path.join(ROOT, "results", "final_ram10_wb")   # canonical: 1 MiB write buffer
SIZES = os.path.join(ROOT, "results", "final_sizes")      # request-size distributions
SEQ = os.path.join(ROOT, "results", "seq")                # sequential 4K writes

# Times-compatible serif (STIXGeneral has Times New Roman metrics).
plt.rcParams.update({
    "font.family": "serif",
    "font.serif": ["STIXGeneral", "Times New Roman", "DejaVu Serif"],
    "mathtext.fontset": "stix",
    "font.size": 19,
    "axes.titlesize": 20,
    "axes.labelsize": 20,
    "xtick.labelsize": 18,
    "ytick.labelsize": 18,
    "legend.fontsize": 16,
    "axes.linewidth": 1.0,
    "figure.dpi": 110,
    "savefig.dpi": 200,
    "savefig.bbox": "tight",
})

ZS = ["0", "0.8", "0.99", "1.2"]
# (file tag, label, color, marker)
POLICIES = [
    ("proposed",   "Proposed",  "#2a78d6", "o"),
    ("page4k_eq",  "Page-4K",   "#eb6834", "s"),
    ("dftl_eq",    "DFTL",      "#1baf7a", "^"),
    ("iu_rmw_eq",  "IU+RMW",    "#eda100", "D"),
    ("mu_eq",      r"$\mu$-FTL", "#e87ba4", "v"),
    ("fast_eq",    "FAST",      "#008300", "P"),
]
# LAST is excluded from the figures (WAF 150-2200 compresses every other curve);
# its numbers stay in the result tables.


def load(d, name):
    with open(os.path.join(d, name)) as f:
        return json.load(f)


def save(fig, stem):
    os.makedirs(OUT, exist_ok=True)
    for ext in ("pdf", "png"):
        path = os.path.join(OUT, f"{stem}.{ext}")
        fig.savefig(path)
    plt.close(fig)
    print("wrote", os.path.join(OUT, stem) + ".{pdf,png}")


def log_ticks(ax, vals):
    """Readable decimal ticks on a narrow log axis."""
    ax.yaxis.set_major_locator(FixedLocator(vals))
    ax.yaxis.set_minor_locator(FixedLocator([]))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))


def outside_legend(ax, ncol, y=1.02):
    """Horizontal legend above the axes."""
    ax.legend(loc="lower center", bbox_to_anchor=(0.5, y), ncol=ncol,
              frameon=False, handlelength=1.6, columnspacing=1.4,
              handletextpad=0.5, borderaxespad=0.0)


def fig_waf_zipf():
    fig, ax = plt.subplots(figsize=(9, 6))
    for tag, label, color, marker in POLICIES:
        y = [load(CANON, f"{tag}_z{z}.json")["WAF"] for z in ZS]
        ax.plot(range(len(ZS)), y, marker=marker, color=color, label=label,
                linewidth=3.0 if tag == "proposed" else 1.8,
                markersize=11 if tag == "proposed" else 8,
                zorder=3 if tag == "proposed" else 2)
    ax.set_yscale("log")
    log_ticks(ax, [3, 5, 10, 20, 50])
    ax.set_ylim(2.5, 60)
    ax.set_xticks(range(len(ZS)))
    ax.set_xticklabels([f"{z}" for z in ZS])
    ax.set_xlabel(r"Zipf skew $\alpha$ (IU rank)")
    ax.set_ylabel("WAF")
    ax.set_xlim(-0.25, len(ZS) - 0.75)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    outside_legend(ax, 3)
    save(fig, "fig1_waf_zipf")


def fig_waf_sizes():
    groups = [("small-heavy\n" + r"$\alpha$=0", "small", "0"),
              ("small-heavy\n" + r"$\alpha$=0.99", "small", "0.99"),
              ("large-heavy\n" + r"$\alpha$=0", "large", "0"),
              ("large-heavy\n" + r"$\alpha$=0.99", "large", "0.99")]
    mods = POLICIES[:4]
    fig, ax = plt.subplots(figsize=(10, 6))
    x = np.arange(len(groups))
    w = 0.2
    for i, (tag, label, color, _) in enumerate(mods):
        key = tag.replace("_eq", "")
        vals = [load(SIZES, f"{key}_{d}_z{z}.json")["WAF"] for _, d, z in groups]
        bars = ax.bar(x + (i - 1.5) * w, vals, w * 0.92, label=label, color=color)
        for b, v in zip(bars, vals):
            ax.annotate(f"{v:.1f}", (b.get_x() + b.get_width() / 2, v),
                        ha="center", va="bottom", fontsize=15, xytext=(0, 2),
                        textcoords="offset points")
    ax.set_yscale("log")
    log_ticks(ax, [1, 2, 5, 10, 20, 40])
    ax.set_ylim(1, 48)
    ax.set_xticks(x)
    ax.set_xticklabels([g[0] for g in groups])
    ax.set_ylabel("WAF")
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    outside_legend(ax, 4)
    save(fig, "fig2_waf_sizes")


def fig_ram():
    fig, ax = plt.subplots(figsize=(9, 5.5))
    labels, vals, colors = [], [], []
    for tag, label, color, _ in POLICIES[:6]:
        m = max(load(CANON, f"{tag}_z{z}.json")["mem"]["total_mib"] for z in ZS)
        labels.append(label)
        vals.append(m)
        colors.append(color)
    order = np.argsort(vals)[::-1]
    labels = [labels[i] for i in order]
    vals = [vals[i] for i in order]
    colors = [colors[i] for i in order]
    bars = ax.bar(labels, vals, color=colors, width=0.62)
    for b, v in zip(bars, vals):
        ax.annotate(f"{v:.1f}", (b.get_x() + b.get_width() / 2, v), ha="center",
                    va="bottom", fontsize=16, xytext=(0, 2), textcoords="offset points")
    ax.set_ylabel("Mapping DRAM (MiB)")
    ax.set_title("64 GiB logical capacity", fontsize=17)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig3_ram")


def fig_breakdown_proposed():
    parts = [("host", "Host", "#2a78d6"),
             ("cgc_clean", "Coarse-tier GC", "#eb6834"),
             ("cgc_refold", "Merge with coarse-tier GC", "#1baf7a"),
             ("fold", "Merge with fine-tier GC", "#eda100"),
             ("finegc", "Fine-tier GC", "#e87ba4")]
    fig, ax = plt.subplots(figsize=(9, 6))
    x = np.arange(len(ZS))
    bottom = np.zeros(len(ZS))
    for key, label, color in parts:
        vals = []
        for z in ZS:
            r = load(CANON, f"proposed_z{z}.json")
            vals.append(r["prog"].get(key, 0) / r["host_pages"])
        ax.bar(x, vals, 0.55, bottom=bottom, label=label, color=color)
        bottom += np.array(vals)
    for xi, tot in zip(x, bottom):
        ax.annotate(f"{tot:.2f}", (xi, tot), ha="center", va="bottom", fontsize=16,
                    xytext=(0, 3), textcoords="offset points")
    ax.set_xticks(x)
    ax.set_xticklabels(ZS)
    ax.set_xlabel(r"Zipf skew $\alpha$ (IU rank)")
    ax.set_ylabel("NAND writes per host page")
    outside_legend(ax, 2)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig4_breakdown_proposed")


def fig_breakdown_sizes():
    """Same write breakdown as fig4, for the small-heavy / large-heavy size mixes."""
    parts = [("host", "Host", "#2a78d6"),
             ("cgc_clean", "Coarse-tier GC", "#eb6834"),
             ("cgc_refold", "Merge with coarse-tier GC", "#1baf7a"),
             ("fold", "Merge with fine-tier GC", "#eda100"),
             ("finegc", "Fine-tier GC", "#e87ba4")]
    groups = [("small-heavy\n" + r"$\alpha$=0", "small", "0"),
              ("small-heavy\n" + r"$\alpha$=0.99", "small", "0.99"),
              ("large-heavy\n" + r"$\alpha$=0", "large", "0"),
              ("large-heavy\n" + r"$\alpha$=0.99", "large", "0.99")]
    fig, ax = plt.subplots(figsize=(9.5, 6))
    x = np.arange(len(groups))
    bottom = np.zeros(len(groups))
    for key, label, color in parts:
        vals = []
        for _, d, z in groups:
            r = load(SIZES, f"proposed_{d}_z{z}.json")
            vals.append(r["prog"].get(key, 0) / r["host_pages"])
        ax.bar(x, vals, 0.55, bottom=bottom, label=label, color=color)
        bottom += np.array(vals)
    for xi, tot in zip(x, bottom):
        ax.annotate(f"{tot:.2f}", (xi, tot), ha="center", va="bottom", fontsize=16,
                    xytext=(0, 3), textcoords="offset points")
    ax.set_xticks(x)
    ax.set_xticklabels([g[0] for g in groups])
    ax.set_ylabel("NAND writes per host page")
    ax.set_ylim(0, max(bottom) * 1.1)
    outside_legend(ax, 2)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig4b_breakdown_sizes")


def fig_breakdown_policies():
    """Write breakdown per policy for the default size mix (canonical setting)."""
    mods = POLICIES[:4]
    zshow = ["0", "0.99"]
    fig, ax = plt.subplots(figsize=(11, 6))
    labels, recs, xs = [], [], []
    for gi, z in enumerate(zshow):
        for mi, (tag, label, _, _) in enumerate(mods):
            recs.append(load(CANON, f"{tag}_z{z}.json"))
            labels.append(label)
            xs.append(gi * (len(mods) + 0.8) + mi)
    tot = _stack(ax, np.array(xs, dtype=float), recs)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=30, ha="right", fontsize=16)
    for gi, z in enumerate(zshow):
        ax.annotate(r"$\alpha$=" + z, (gi * (len(mods) + 0.8) + (len(mods) - 1) / 2, 0),
                    xytext=(0, -68), textcoords="offset points", ha="center", fontsize=18)
    ax.set_ylabel("NAND writes per host page")
    ax.set_ylim(0, max(tot) * 1.1)
    outside_legend(ax, 4)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig5_breakdown_policies")


# Unified write-source stack used by the policy-comparison figures.
# page4k / DFTL map everything at 4 KB, so their GC is counted as fine-tier GC.
STACK = [("host",       ["host"],               "Host",                      "#2a78d6"),
         ("rmw",        ["rmw"],                "RMW",                       "#7a5195"),
         ("coarse",     ["cgc_clean"],          "Coarse-tier GC",            "#eb6834"),
         ("refold",     ["cgc_refold"],         "Merge with coarse-tier GC", "#1baf7a"),
         ("fold",       ["fold", "unfold"],     "Merge with fine-tier GC",   "#eda100"),
         ("fine",       ["finegc", "pgc"],      "Fine-tier GC",              "#e87ba4"),
         ("map",        ["map"],                "Map write",                 "#4a3aa7")]


def _stack(ax, xs, recs, width=0.6, annotate=True, stack=None, legend=True):
    bottom = np.zeros(len(recs))
    for _, keys, label, color in (stack or STACK):
        vals = np.array([sum(r["prog"].get(k, 0) for k in keys) / r["host_pages"] for r in recs])
        if vals.max() > 0:
            ax.bar(xs, vals, width, bottom=bottom, color=color, label=label if legend else None)
        bottom += vals
    if annotate:
        for x, t in zip(xs, bottom):
            ax.annotate(f"{t:.1f}", (x, t), ha="center", va="bottom", fontsize=14,
                        xytext=(0, 2), textcoords="offset points")
    return bottom


def fig_breakdown_sizes_policies():
    """Write breakdown per policy for the three request-size mixes."""
    mods = POLICIES[:4]
    panels = [(CANON, "",      "default mix\n(4 KB-256 KB, uniform by request)"),
              (SIZES, "small", "small-heavy\n(50% 4 KB requests)"),
              (SIZES, "large", "large-heavy\n(50% 256 KB requests)")]
    fig, axes = plt.subplots(1, 3, figsize=(19, 6.2), sharey=True)   # common y axis across panels
    for ax, (d, dist, title) in zip(axes, panels):
        labels, recs, xs = [], [], []
        for gi, z in enumerate(["0", "0.99"]):
            for mi, (tag, label, _, _) in enumerate(mods):
                name = f"{tag}_z{z}.json" if not dist else f"{tag.replace('_eq','')}_{dist}_z{z}.json"
                recs.append(load(d, name))
                labels.append(label)
                xs.append(gi * (len(mods) + 0.8) + mi)
        _stack(ax, np.array(xs, dtype=float), recs)
        ax.set_xticks(xs)
        ax.set_xticklabels(labels, rotation=30, ha="right", fontsize=15)
        ax.set_ylim(0, 37)
        ax.set_title(title, fontsize=17)
        ax.grid(axis="y", color="#dddddd", linewidth=0.8)
        ax.set_axisbelow(True)
        for gi, z in enumerate(["0", "0.99"]):
            ax.annotate(r"$\alpha$=" + z, (gi * (len(mods) + 0.8) + (len(mods) - 1) / 2, 0),
                        xytext=(0, -62), textcoords="offset points", ha="center", fontsize=17)
    axes[0].set_ylabel("NAND writes per host page")
    h, l = [], []
    for ax in axes:
        for hh, ll in zip(*ax.get_legend_handles_labels()):
            if ll not in l:
                h.append(hh)
                l.append(ll)
    fig.legend(h, l, loc="lower center", bbox_to_anchor=(0.5, 0.97), ncol=4, frameon=False,
               handlelength=1.6, columnspacing=1.4, handletextpad=0.5)
    save(fig, "fig5b_breakdown_sizes_policies")

# Write sources of the legacy hybrid / extent baselines (merge-based, no fine/coarse tiers).
STACK_LEGACY = [("host",    ["host"],         "Host",           "#2a78d6"),
                ("full",    ["fullmerge"],    "Full merge",     "#eb6834"),
                ("partial", ["partialmerge"], "Partial merge",  "#eda100"),
                ("gc",      ["pgc"],          "GC relocation",  "#e87ba4"),
                ("map",     ["map"],          "Map write",      "#4a3aa7")]


def fig_breakdown_legacy():
    """Write breakdown of FAST and mu-FTL (LAST is 198-2205 and left out of the plot)."""
    pol = [("fast", "FAST"), ("mu", r"$\mu$-FTL")]
    fig, axes = plt.subplots(1, 2, figsize=(15, 6.2), sharey=True,
                             gridspec_kw={"width_ratios": [1.15, 1]})
    # left: default mix over the zipf sweep
    ax = axes[0]
    labels, recs, xs = [], [], []
    for gi, z in enumerate(ZS):
        for mi, (key, label) in enumerate(pol):
            recs.append(load(CANON, f"{key}_eq_z{z}.json"))
            labels.append(label)
            xs.append(gi * (len(pol) + 0.7) + mi)
    _stack(ax, np.array(xs, dtype=float), recs, width=0.75, stack=STACK_LEGACY)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=30, ha="right", fontsize=15)
    for gi, z in enumerate(ZS):
        ax.annotate(r"$\alpha$=" + z, (gi * (len(pol) + 0.7) + (len(pol) - 1) / 2, 0),
                    xytext=(0, -58), textcoords="offset points", ha="center", fontsize=16)
    ax.set_title("default mix, Zipf sweep", fontsize=17)
    ax.set_ylabel("NAND writes per host page")
    # right: size mixes
    ax = axes[1]
    groups = [("small\n" + r"$\alpha$=0", "small", "0"), ("small\n" + r"$\alpha$=0.99", "small", "0.99"),
              ("large\n" + r"$\alpha$=0", "large", "0"), ("large\n" + r"$\alpha$=0.99", "large", "0.99")]
    labels, recs, xs = [], [], []
    for gi, (_, dist, z) in enumerate(groups):
        for mi, (key, label) in enumerate(pol):
            recs.append(load(SIZES, f"{key}_{dist}_z{z}.json"))
            labels.append(label)
            xs.append(gi * (len(pol) + 0.7) + mi)
    _stack(ax, np.array(xs, dtype=float), recs, width=0.75, stack=STACK_LEGACY, legend=False)
    ax.set_xticks(xs)
    ax.set_xticklabels(labels, rotation=30, ha="right", fontsize=15)
    for gi, (gname, _, _) in enumerate(groups):
        ax.annotate(gname, (gi * (len(pol) + 0.7) + (len(pol) - 1) / 2, 0),
                    xytext=(0, -70), textcoords="offset points", ha="center", fontsize=16)
    ax.set_title("request-size mixes", fontsize=17)
    for ax in axes:
        ax.set_ylim(0, 75)
        ax.grid(axis="y", color="#dddddd", linewidth=0.8)
        ax.set_axisbelow(True)
    h, l = axes[0].get_legend_handles_labels()
    fig.legend(h, l, loc="lower center", bbox_to_anchor=(0.5, 0.96), ncol=5, frameon=False,
               handlelength=1.6, columnspacing=1.4, handletextpad=0.5)
    save(fig, "fig5c_breakdown_legacy")


def fig_seq():
    mods = POLICIES[:4]
    fig, ax = plt.subplots(figsize=(9, 5.5))
    x = np.arange(len(mods))
    w = 0.36
    nob = [load(SEQ, f"{t.replace('_eq','')}_nobuf.json")["WAF"] for t, _, _, _ in mods]
    wb = [load(SEQ, f"{t.replace('_eq','')}_wb1M.json")["WAF"] for t, _, _, _ in mods]
    b1 = ax.bar(x - w / 2, nob, w * 0.92, label="no write buffer", color="#eb6834")
    b2 = ax.bar(x + w / 2, wb, w * 0.92, label="1 MiB write buffer", color="#2a78d6")
    for bars, vals in ((b1, nob), (b2, wb)):
        for b, v in zip(bars, vals):
            ax.annotate(f"{v:.2f}", (b.get_x() + b.get_width() / 2, v), ha="center",
                        va="bottom", fontsize=15, xytext=(0, 2), textcoords="offset points")
    ax.set_yscale("log")
    log_ticks(ax, [1, 2, 5, 10, 20])
    ax.set_ylim(0.85, 30)
    ax.set_xticks(x)
    ax.set_xticklabels([m[1] for m in mods])
    ax.set_ylabel("WAF")
    outside_legend(ax, 2)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig6_seq_4k")


def fig_waf_zipf_main():
    """Same data as fig1 but only the practical designs, linear scale (headline figure)."""
    fig, ax = plt.subplots(figsize=(9, 6))
    for tag, label, color, marker in POLICIES[:4]:
        y = [load(CANON, f"{tag}_z{z}.json")["WAF"] for z in ZS]
        ax.plot(range(len(ZS)), y, marker=marker, color=color, label=label,
                linewidth=3.0 if tag == "proposed" else 2.0,
                markersize=12 if tag == "proposed" else 9,
                zorder=3 if tag == "proposed" else 2)
    ax.set_xticks(range(len(ZS)))
    ax.set_xticklabels(ZS)
    ax.set_xlabel(r"Zipf skew $\alpha$ (IU rank)")
    ax.set_ylabel("WAF")
    ax.set_xlim(-0.15, len(ZS) - 0.85)
    ax.set_ylim(0, 13.5)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    outside_legend(ax, 4)
    save(fig, "fig1b_waf_zipf_main")


def fig_map_read():
    """DFTL pays synchronous mapping reads on the host path; the proposal pays none."""
    fig, ax = plt.subplots(figsize=(9, 5.5))
    x = np.arange(len(ZS))
    w = 0.38
    dftl = [load(CANON, f"dftl_eq_z{z}.json")["map_miss"] /
            load(CANON, f"dftl_eq_z{z}.json")["host_pages"] for z in ZS]
    prop = [0.0 for _ in ZS]
    b1 = ax.bar(x - w / 2, dftl, w * 0.92, label="DFTL", color="#1baf7a")
    b2 = ax.bar(x + w / 2, prop, w * 0.92, label="Proposed", color="#2a78d6")
    for b, v in zip(b1, dftl):
        ax.annotate(f"{v:.2f}", (b.get_x() + b.get_width() / 2, v), ha="center",
                    va="bottom", fontsize=16, xytext=(0, 2), textcoords="offset points")
    for b in b2:
        ax.annotate("0", (b.get_x() + b.get_width() / 2, 0), ha="center", va="bottom",
                    fontsize=16, xytext=(0, 2), textcoords="offset points", color="#2a78d6")
    ax.set_xticks(x)
    ax.set_xticklabels(ZS)
    ax.set_xlabel(r"Zipf skew $\alpha$ (IU rank)")
    ax.set_ylabel("Synchronous map reads\nper host page")
    ax.set_ylim(0, max(dftl) * 1.15)
    outside_legend(ax, 2)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig7_map_read")


# ---------------------------------------------------------------- related work table
HDR_BG, HDR_FG = "#33627d", "#ffffff"
ROW_BG = ("#ccd1d5", "#e3e8eb")

# Feature keys (the proposal satisfies all of them):
#  sub   : coarse mapping entry covers less than one erase block, so a merge writes << one block
#  dual  : the SAME logical range is mapped at 4 KB and at coarse granularity at once,
#          the 4 KB copy superseding part of the coarse unit (not a mode switch, not disjoint
#          regions, not medium tiering)
#  cost  : an explicit write-cost comparison decides "reclaim the 4 KB area" vs. "merge into coarse"
#  gcmrg : the coarse area is garbage collected, and the cross-tier merge rides on that
#          relocation for free (no extra program)
#  dram  : the whole mapping is DRAM-resident (no synchronous map read from flash on the I/O path)
FEATS = {
    "sub":   "Sub-block\ncoarse unit",
    "dual":  "Coexisting\n4 KB map",
    "cost":  "Cost-based\nGC vs. merge",
    "gcmrg": "Merge in\ncoarse GC",
    "dram":  "In-DRAM\nmap",
}
# 1 = has it, 0 = does not
RW = [
    ("BAST / FAST / LAST",  dict(sub=0, dual=1, cost=0, gcmrg=0, dram=1)),
    ("Superblock FTL",      dict(sub=0, dual=1, cost=0, gcmrg=0, dram=1)),
    ("Janus-FTL",           dict(sub=0, dual=1, cost=1, gcmrg=0, dram=1)),
    ("CFTL",                dict(sub=0, dual=0, cost=0, gcmrg=0, dram=0)),
    ("DFTL / LeaFTL",       dict(sub=0, dual=0, cost=0, gcmrg=0, dram=0)),
    (r"$\mu$-FTL",          dict(sub=1, dual=0, cost=0, gcmrg=0, dram=0)),
    ("Subpage-aware SSD",   dict(sub=1, dual=1, cost=0, gcmrg=0, dram=1)),
    ("MGA-FTL",             dict(sub=1, dual=1, cost=0, gcmrg=0, dram=1)),
    ("SLC cache (device)",  dict(sub=0, dual=0, cost=0, gcmrg=0, dram=1)),
    ("CSAL (host-side)",    dict(sub=0, dual=0, cost=0, gcmrg=0, dram=0)),
    ("IU + RMW (QLC)",      dict(sub=1, dual=0, cost=0, gcmrg=0, dram=1)),
    ("Proposed",            dict(sub=1, dual=1, cost=1, gcmrg=1, dram=1)),
]
COMPACT_KEYS = ["sub", "dual", "cost", "gcmrg", "dram"]
COMPACT_ROWS = ["FAST", "Janus-FTL", r"$\mu$-FTL", "Proposed"]


def _table(rows, keys, stem, label_w=0.22, row_h=0.085, hdr_h=0.15, figsize=(14, 6.0)):
    fig, ax = plt.subplots(figsize=figsize)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, hdr_h + row_h * len(rows))
    ax.axis("off")
    cw = (1.0 - label_w) / len(keys)
    top = hdr_h + row_h * len(rows)
    ax.add_patch(plt.Rectangle((label_w, top - hdr_h), 1 - label_w, hdr_h,
                               facecolor=HDR_BG, edgecolor="white", linewidth=1.5))
    for j, k in enumerate(keys):
        ax.text(label_w + cw * (j + 0.5), top - hdr_h / 2, FEATS[k], color=HDR_FG,
                fontsize=15, fontweight="bold", va="center", ha="center", linespacing=1.3)
    for i, (name, f) in enumerate(rows):
        y = top - hdr_h - row_h * (i + 1)
        last = (i == len(rows) - 1)
        ax.add_patch(plt.Rectangle((0, y), 1, row_h,
                                   facecolor="#b9d3e6" if last else ROW_BG[i % 2],
                                   edgecolor="white", linewidth=1.5))
        ax.text(0.012, y + row_h / 2, name, fontsize=17, va="center", ha="left",
                fontweight="bold" if last else "normal")
        for j, k in enumerate(keys):
            ax.text(label_w + cw * (j + 0.5), y + row_h / 2, "O" if f[k] else "X",
                    fontsize=19, va="center", ha="center",
                    color="#0b0b0b" if f[k] else "#777777")
    save(fig, stem)


def fig_related_work():
    _table(RW, list(FEATS), "fig8_related_work")
    compact = [(n, f) for n, f in RW if n in COMPACT_ROWS or n.startswith("BAST")]
    compact = [(("FAST" if n.startswith("BAST") else n), f) for n, f in compact]
    _table(compact, COMPACT_KEYS, "fig8b_related_work_compact",
           row_h=0.13, hdr_h=0.20, figsize=(12, 4.4))


ABL = os.path.join(ROOT, "results", "ablation")


def fig_ablation():
    """2x2 ablation: write-time selective RMW and fine-tier fold."""
    cfg = [("neither", "neither", "#b9bcc0"),
           ("nofold",  "write-time selective RMW only", "#eda100"),
           ("nowrite", "merge with fine-tier GC only", "#1baf7a"),
           ("full",    "both (proposed)", "#2a78d6")]
    wl = [("def_z0", "default\n" + r"$\alpha$=0"), ("def_z0.8", "default\n" + r"$\alpha$=0.8"),
          ("def_z0.99", "default\n" + r"$\alpha$=0.99"), ("def_z1.2", "default\n" + r"$\alpha$=1.2"),
          ("small_z0", "small\n" + r"$\alpha$=0"), ("small_z0.99", "small\n" + r"$\alpha$=0.99"),
          ("large_z0", "large\n" + r"$\alpha$=0")]
    fig, ax = plt.subplots(figsize=(13, 6))
    x = np.arange(len(wl), dtype=float)
    w = 0.2
    for i, (key, label, color) in enumerate(cfg):
        vals = [load(ABL, f"{key}_{k}.json")["WAF"] for k, _ in wl]
        bars = ax.bar(x + (i - 1.5) * w, vals, w * 0.9, label=label, color=color)
        for b, v in zip(bars, vals):
            ax.annotate(f"{v:.1f}", (b.get_x() + b.get_width() / 2, v), ha="center",
                        va="bottom", fontsize=13, xytext=(0, 2), textcoords="offset points")
    ax.set_xticks(x)
    ax.set_xticklabels([n for _, n in wl], fontsize=16)
    ax.set_ylabel("WAF")
    ax.set_ylim(0, 21)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    outside_legend(ax, 2)
    save(fig, "fig9_ablation")


if __name__ == "__main__":
    fig_waf_zipf()
    fig_waf_zipf_main()
    fig_waf_sizes()
    fig_ram()
    fig_breakdown_proposed()
    fig_breakdown_sizes()
    fig_breakdown_policies()
    fig_breakdown_sizes_policies()
    fig_breakdown_legacy()
    fig_seq()
    fig_map_read()
    fig_related_work()
    fig_ablation()
