#!/usr/bin/env python3
"""DUET mapping structures (results/paper_dac/fig_design.{pdf,png}, also paper/figs/fig_design.pdf), single column.
Same drawing rules as the intro figure (tools/plot_intro.py): black outlines, NAND blocks of 8 pages, a coarse IU =
4 pages, pink = mapping entries / map pages, purple = coarse-IU-mapped (chunks), blue = page-mapped (fine pages),
H brackets mark the unit one entry points to.
Input host LBA -> LPN -> i = LPN / U, k = LPN mod U. DRAM: coarse map (zoomed 4 B entry: chunk index, F, S, C), fine map
(2 candidate buckets of 4 entries, zoomed 8 B entry), GTD (one 4 B entry per FTP). NAND: chunk, fine page, FTP
(zoomed: 1024 4 B pointers = 1024/U coarse IUs x U). Arrows: F = 0 -> chunk page k; F = 1 -> table lookup; S = 1 -> GTD -> FTP slot;
both candidate buckets full -> spill (dashed).
The previous overview version (blocks, merge during fine / coarse GC) is in git history."""
import shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch
from plot_intro import (Blk, point, arrow, lab, hbar, K, C_BAD, C_AR, F_MAP, F_CO, F_PG, F_FREE, FS, SF, PW, PH)

OUT = '/home/sejun000/dual_mapping/results/paper_dac/fig_design'
PAPER = '/home/sejun000/dual_mapping/paper/figs/fig_design.pdf'
plt.rcParams.update({'font.size': FS, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4})

XMAX, YMAX = 20.0, 11.2   # was 11.75 (2026-11-13: compact)
DX0, DX1, DY0, DY1 = 0.5, 19.7, 4.75, 10.45    # DRAM budget box
NX0, NX1, NY0, NY1 = 0.5, 19.7, 0.05, 4.45    # NAND box
EW, EH = 0.52, 0.6                            # map-entry cell
EY = 8.45                                      # entry row (bottom y)
LY = 10.05                                     # structure labels
NR = 2.9                                      # NAND block row (bottom y)
ZL = dict(color='#888888', lw=0.4, ls=(0, (2, 1.5)))   # zoom lines


def cells(ax, x, y, w, h, n, fc=F_MAP, lw=0.4):
    for j in range(n):
        ax.add_patch(Rectangle((x + j * w, y), w, h, fc=fc, ec=K, lw=lw))


def fields(ax, x, y, h, spec):
    """zoomed entry: spec = [(width, text, face)]; returns x of each field's center."""
    cx = []
    for w, t, fc in spec:
        ax.add_patch(Rectangle((x, y), w, h, fc=fc, ec=K, lw=0.6))
        ax.text(x + w / 2, y + h / 2, t, ha='center', va='center', fontsize=SF - 0.5)
        cx.append(x + w / 2); x += w
    return cx


def num(ax, x, y, s, fs=SF - 1.2):
    """numbered read case (legend at the bottom left)."""
    ax.text(x, y, s, fontsize=fs, color='white', ha='center', va='center_baseline', fontweight='bold',
            bbox=dict(boxstyle='circle,pad=0.1', fc=C_AR, ec='none'))


def zoom(ax, a0, a1, b0, b1):
    """dashed zoom lines from the small cell edge points a0/a1 to the big box edge points b0/b1."""
    for p, q in ((a0, b0), (a1, b1)):
        ax.plot([p[0], q[0]], [p[1], q[1]], **ZL)


def main():
    fig, ax = plt.subplots(figsize=(3.4, 3.4 * YMAX / XMAX))
    ax.set_xlim(0, XMAX); ax.set_ylim(0, YMAX); ax.axis('off')
    ax.add_patch(Rectangle((DX0, DY0), DX1 - DX0, DY1 - DY0, fc='none', ec=K, lw=0.6, ls=(0, (3, 2))))
    ax.add_patch(Rectangle((NX0, NY0), NX1 - NX0, NY1 - NY0, fc='#F7F7F5', ec=K, lw=0.6))
    ax.text(0.2, (DY0 + DY1) / 2, 'DRAM', rotation=90, ha='center', va='center', fontsize=SF - 0.4)
    ax.text(0.2, (NY0 + NY1) / 2, 'NAND', rotation=90, ha='center', va='center', fontsize=SF - 0.4)

    # ---- coarse map: 8 entries, entry 2 = coarse IU i, zoomed below as [chunk index | F | S | C]
    cx0, ci = 0.9, 2
    cells(ax, cx0, EY, EW, EH, 8)
    ax.add_patch(Rectangle((cx0 + ci * EW, EY), EW, EH, fc='none', ec=K, lw=1.2))
    ix = cx0 + (ci + 0.5) * EW
    lab(ax, 0.9, 10.95, r'host LBA $\rightarrow$ LPN:  $i=\lfloor \mathrm{LPN}/U \rfloor$,  $k=\mathrm{LPN}\ \mathrm{mod}\ U$   ($U$: pages per coarse IU)',
        fontsize=SF - 0.4)
    arrow(ax, (ix, 10.7), (ix, EY + EH + 0.05))
    lab(ax, ix + 0.3, LY - 0.25, 'coarse map', fontweight='bold')
    lab(ax, ix + 0.3, LY - 0.75, 'entry $i$', fontsize=SF - 0.6)
    zx, zy, zh, ZW = 0.9, 6.3, 0.55, 4.5                                  # zoom row: chunk index, LPN / PPN, GTD
    f = fields(ax, zx, zy, zh, [(2.85, 'chunk index', F_MAP), (0.55, 'F', F_MAP), (0.55, 'S', F_MAP), (0.55, 'C', F_MAP)])
    zoom(ax, (cx0 + ci * EW, EY), (cx0 + (ci + 1) * EW, EY), (zx, zy + zh), (zx + ZW, zy + zh))
    hbar(ax, zx, zx + ZW, zy + zh)                               # entry size
    lab(ax, zx + ZW / 2, zy + zh + 0.45, '4B', ha='center', fontsize=SF - 0.7)

    # ---- fine map: 9 buckets x 4 entries, candidate buckets h1 / h2 framed, zoomed entry below
    fx0, BW, SH, NB, FY = 7.9, 0.58, 0.36, 9, 7.65
    h1, h2, used = 1, 6, [3, 4, 2, 4, 1, 3, 4, 2, 3]
    global VICTIM
    VICTIM = [(1, 0), (4, 0), (6, 3)]                              # victim coarse IU: entries in h1, h2 and elsewhere
    for b in range(NB):
        for w in range(4):
            ax.add_patch(Rectangle((fx0 + b * BW, FY + w * SH), BW, SH, fc=F_MAP if w < used[b] else F_FREE,
                                   ec=K, lw=0.3))
        ax.add_patch(Rectangle((fx0 + b * BW, FY), BW, 4 * SH, fc='none', ec=K, lw=0.5))
    for b, s in ((h1, r'$h_1$'), (h2, r'$h_2$')):
        ax.add_patch(Rectangle((fx0 + b * BW, FY), BW, 4 * SH, fc='none', ec=K, lw=0.9))
        lab(ax, fx0 + (b + 0.5) * BW, FY - 0.3, s, ha='center', fontsize=SF - 0.5)
    lab(ax, fx0, LY - 0.25, 'fine map (hash)', fontweight='bold')
    lab(ax, fx0, LY - 0.75, r'2 buckets $\times$ 4 entries per LPN', fontsize=SF - 0.6)
    zx2 = fx0
    fz = fields(ax, zx2, zy, zh, [(1.6, 'LPN', F_MAP), (1.6, 'PPN', F_MAP)])
    sx, sy = fx0 + h1 * BW, FY + 2 * SH                            # zoomed entry: bucket h1, way 2
    ax.add_patch(Rectangle((sx + 0.04, FY + 2 * SH + 0.03), BW - 0.08, SH - 0.06, fc='none', ec=K, lw=1.3))
    zoom(ax, (sx, sy), (sx + BW, sy), (zx2, zy + zh), (zx2 + 3.2, zy + zh))
    hbar(ax, zx2, zx2 + 3.2, zy + zh)
    lab(ax, zx2 + 1.6, zy + zh + 0.45, '8B', ha='center', fontsize=SF - 0.7)

    # ---- GTD: one 4 B entry per FTP (entry 0 = FTP of coarse IU i), on the zoom row
    gx0 = 15.1
    cells(ax, gx0, zy, EW, zh, 7)
    ax.add_patch(Rectangle((gx0, zy), EW, zh, fc='none', ec=K, lw=1.2))
    lab(ax, gx0 - 0.4, zy + zh + 0.85, 'GTD (4B per FTP)', fontweight='bold')
    lab(ax, gx0 - 0.4, zy + zh + 0.35, r'entry $\lfloor \mathrm{LPN}/1024 \rfloor$', fontsize=SF - 0.6)

    # ---- F = 1, S = 0: lookup in the fine map;  F = 1, S = 1: GTD -> FTP (lane under the DRAM structures)
    arrow(ax, (zx + ZW, zy + zh / 2), (fx0 - 0.05, FY + 2 * SH), rad=0.3)
    ly, lx = 5.3, 14.6
    ax.plot([f[2], f[2], lx, lx], [zy, ly, ly, zy + zh / 2], color=C_AR, lw=0.6)
    arrow(ax, (lx, zy + zh / 2), (gx0, zy + zh / 2))
    num(ax, 12.0, ly + 0.33, '4')

    # ---- NAND: coarse block (chunk of coarse IU i), fine block (fine page), map block (FTPs)
    BP, BH = 0.31, 0.7                                              # page width / height: 16-page blocks
    A = Blk(ax, 0.8, NR, [F_CO] * 16, hatch=(1, 4, 8, 9, 10, 11), iu_split=True, pw=BP, ph=BH)   # chunk 0 and 1 (coarse IU i) partly, 2 fully invalid
    lab(ax, A.x + A.w / 2, NR - 0.4, 'coarse block', ha='center')
    ax.add_patch(Rectangle((17.6, 0.35), BP, BH, fc='#E0E0E0', ec=K, lw=0.35, hatch='//////'))   # legend
    lab(ax, 17.6 + BP + 0.2, 0.35 + BH / 2, 'invalid', fontsize=SF - 0.7)
    for j, t in enumerate(('F=0: $k$-th page of chunk', 'F=1, S=0, found in fine map: fine page',
                           'F=1, S=0, not found: $k$-th page of chunk', 'F=1, S=1: FTP slot via GTD')):
        y = 1.95 - 0.5 * j                                           # read cases, numbered on the arrows
        num(ax, 1.05, y, str(j + 1), SF - 0.9)
        ax.text(1.45, y, t, va='center_baseline', fontsize=SF - 0.3)
    Fb = Blk(ax, 7.7, NR, [F_PG] * 12 + [F_FREE] * 4, hatch=(3, 5, 9), pw=BP, ph=BH)
    lab(ax, Fb.x + Fb.w / 2, NR - 0.4, 'fine block', ha='center')
    T = Blk(ax, 14.5, NR, [F_MAP] * 6 + [F_FREE] * 10, pw=BP, ph=BH)
    lab(ax, T.x + T.w / 2, NR - 0.4, 'FTPs', ha='center')
    point(ax, (f[0] + 0.3, zy), A, 4, 4)                            # chunk index -> the whole chunk (coarse IU i)
    ax.add_patch(Rectangle((A.x + 6 * BP, NR), BP, BH, fc='none', ec=K, lw=1.2))   # + offset k -> page k
    ax.text(A.x + 6.5 * BP, NR + BH / 2, '$k$', ha='center', va='center', fontsize=SF - 1.2)
    num(ax, 3.05, 5.75, '1'); num(ax, 3.6, 5.75, '3')
    lab(ax, A.x + 8 * BP + 0.2, NR + BH + 0.3, r'chunk index $\times\ U+k$', fontsize=SF - 0.7)
    point(ax, (fz[1], zy), Fb, 2)                                   # table hit: PPN -> fine page
    tx, ty = Fb.x + 2.5 * BP, NR + BH + 0.29                      # (2) next to the PPN -> fine page arrow
    num(ax, fz[1] - (fz[1] - tx) * (zy - 5.8) / (zy - ty) - 0.3, 5.8, '2')
    point(ax, (gx0 + 0.5 * EW, zy), T, 1)                           # GTD entry -> FTP
    # spill: both candidate buckets full -> all table entries of a victim coarse IU (red, spread over buckets)
    # go to its row of the FTP
    for b, w in VICTIM:
        ax.add_patch(Rectangle((fx0 + b * BW + 0.03, FY + w * SH + 0.03), BW - 0.06, SH - 0.06, fc='none',
                               ec=C_BAD, lw=1.0))
    vb, vw = VICTIM[-1]
    ax.add_patch(FancyArrowPatch((fx0 + (vb + 1) * BW, FY + (vw + 0.5) * SH), T.top(3), arrowstyle='-|>',
                                 mutation_scale=4.5, color=C_BAD, lw=0.6, ls=(0, (2, 1.2)),
                                 connectionstyle='arc3,rad=-0.3', shrinkA=0, shrinkB=0.3))
    lab(ax, 13.5, 8.6, '$h_1$, $h_2$ full: spill\nvictim coarse IU', C_BAD, fontsize=SF - 0.6, fontweight='bold',
        linespacing=0.95)

    # ---- FTP zoomed: 1024/U coarse IUs x U pointers (one 4 KiB page), slot LPN mod 1024
    qx, qy, qw, qh, R, C = 11.25, 0.25, 0.9, 0.34, 5, 4   # a row = one coarse IU (U = 4 in the drawing)
    for r in range(R):
        for c in range(C):
            hit = (r, c) == (1, 2)
            ax.add_patch(Rectangle((qx + c * qw, qy + (R - 1 - r) * qh), qw, qh, fc=F_MAP if hit or (r, c) in
                                   ((0, 1), (2, 0), (2, 3), (3, 0), (3, 1), (3, 3)) else F_FREE, ec=K,
                                   lw=1.2 if hit else 0.3))
    ax.add_patch(Rectangle((qx, qy), C * qw, R * qh, fc='none', ec=K, lw=0.6))
    ax.add_patch(Rectangle((qx + 0.03, qy + (R - 4) * qh + 0.03), C * qw - 0.06, qh - 0.06, fc='none', ec=C_BAD,
                           lw=1.0))                                  # row of the victim coarse IU
    zoom(ax, (T.x + BP, NR), (T.x + 2 * BP, NR), (qx, qy + R * qh), (qx + C * qw, qy + R * qh))
    lab(ax, qx + C * qw + 0.35, qy + R * qh - 0.25, 'coarse IUs\n' + r'$\times\ U$ entries (4 KB)',
        fontsize=SF - 0.7, linespacing=0.95)

    fig.subplots_adjust(left=0.0, right=1.0, top=1.0, bottom=0.0)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}.{ext}', dpi=300, bbox_inches='tight', pad_inches=0.01)
    shutil.copy(OUT + '.pdf', PAPER)
    print('wrote', OUT + '.{pdf,png}', 'and', PAPER)


if __name__ == '__main__':
    main()
