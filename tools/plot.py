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
             ("cgc_clean", "GC relocation (clean)", "#eb6834"),
             ("cgc_refold", "GC relocation (refold)", "#1baf7a"),
             ("fold", "Fold", "#eda100"),
             ("finegc", "Fine-tier copy", "#e87ba4")]
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
    outside_legend(ax, 3)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig4_breakdown_proposed")


def fig_breakdown_policies():
    mods = POLICIES[:4]
    gc_keys = ["pgc", "cgc_clean", "cgc_refold", "finegc", "fold", "unfold",
               "fullmerge", "partialmerge", "zerofill"]
    fig, ax = plt.subplots(figsize=(10, 6))
    zshow = ["0", "0.99"]
    x = np.arange(len(mods) * len(zshow))
    labels, host, rmw, gc, mp = [], [], [], [], []
    for zi, z in enumerate(zshow):
        for tag, label, _, _ in mods:
            r = load(CANON, f"{tag}_z{z}.json")
            h = r["host_pages"]
            labels.append(f"{label}\n" + r"$\alpha$=" + z)
            host.append(r["prog"]["host"] / h)
            rmw.append(r["prog"].get("rmw", 0) / h)
            gc.append(sum(r["prog"].get(k, 0) for k in gc_keys) / h)
            mp.append(r["prog"].get("map", 0) / h)
    host, rmw, gc, mp = map(np.array, (host, rmw, gc, mp))
    ax.bar(x, host, 0.6, label="Host", color="#2a78d6")
    ax.bar(x, rmw, 0.6, bottom=host, label="RMW", color="#eda100")
    ax.bar(x, gc, 0.6, bottom=host + rmw, label="GC / merge", color="#eb6834")
    ax.bar(x, mp, 0.6, bottom=host + rmw + gc, label="Map write", color="#1baf7a")
    tot = host + rmw + gc + mp
    for xi, t in zip(x, tot):
        ax.annotate(f"{t:.1f}", (xi, t), ha="center", va="bottom", fontsize=15,
                    xytext=(0, 3), textcoords="offset points")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=15)
    ax.set_ylabel("NAND writes per host page")
    outside_legend(ax, 4)
    ax.set_ylim(0, max(tot) * 1.12)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    save(fig, "fig5_breakdown_policies")


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

# Columns: what each design does.
#  1. coarse mapping unit finer than an erase block (IU / extent, not a whole block)
#  2. cost model that chooses between reclaiming the 4K area and merging into the coarse area
#  3. a merge operation that shrinks the fine mapping table
#  4. garbage collection inside the coarse-mapped area itself
RW_COLS = ["Sub-block coarse\nmapping unit",
           "GC vs. merge\ncost model",
           "Map shrink\nby merge",
           "Coarse-area\nGC"]
RW_ROWS = [
    ("FAST",        ["X", "X", "O", "X"]),
    ("Janus-FTL",   ["X", "O", "O", "X"]),
    (r"$\mu$-FTL",  ["O", "X", "X", "O"]),
    ("Proposed",    ["O", "O", "O", "O"]),
]
RW_ROWS_EXT = [
    ("FAST / LAST", ["X", "X", "O", "X"]),
    ("Superblock",  ["X", "X", "O", "X"]),
    ("Janus-FTL",   ["X", "O", "O", "X"]),
    ("DFTL / CFTL", ["X", "X", "X", "O"]),
    (r"$\mu$-FTL",  ["O", "X", "X", "O"]),
    ("IU + RMW",    ["O", "X", "X", "O"]),
    ("Proposed",    ["O", "O", "O", "O"]),
]


def _table(rows, cols, stem, label_w=0.22, row_h=0.115, hdr_h=0.20, figsize=(13, 4.4)):
    fig, ax = plt.subplots(figsize=figsize)
    ax.set_xlim(0, 1)
    ax.set_ylim(0, hdr_h + row_h * len(rows))
    ax.axis("off")
    cw = (1.0 - label_w) / len(cols)
    top = hdr_h + row_h * len(rows)
    ax.add_patch(plt.Rectangle((label_w, top - hdr_h), 1 - label_w, hdr_h,
                               facecolor=HDR_BG, edgecolor="white", linewidth=1.5))
    for j, c in enumerate(cols):
        ax.text(label_w + cw * (j + 0.5), top - hdr_h / 2, c, color=HDR_FG,
                fontsize=17, fontweight="bold", va="center", ha="center",
                linespacing=1.3)
    for i, (name, marks) in enumerate(rows):
        y = top - hdr_h - row_h * (i + 1)
        ax.add_patch(plt.Rectangle((0, y), 1, row_h, facecolor=ROW_BG[i % 2],
                                   edgecolor="white", linewidth=1.5))
        ax.text(0.012, y + row_h / 2, name, fontsize=19, va="center", ha="left")
        for j, m in enumerate(marks):
            ax.text(label_w + cw * (j + 0.5), y + row_h / 2, m, fontsize=20,
                    va="center", ha="center",
                    color="#0b0b0b" if m == "O" else "#777777")
    save(fig, stem)


def fig_related_work():
    _table(RW_ROWS, RW_COLS, "fig8_related_work")
    _table(RW_ROWS_EXT, RW_COLS, "fig8b_related_work_ext",
           row_h=0.085, hdr_h=0.15, figsize=(13, 5.6))


if __name__ == "__main__":
    fig_waf_zipf()
    fig_waf_zipf_main()
    fig_waf_sizes()
    fig_ram()
    fig_breakdown_proposed()
    fig_breakdown_policies()
    fig_seq()
    fig_map_read()
    fig_related_work()
