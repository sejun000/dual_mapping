#!/usr/bin/env python3
"""Intro figure (results/paper_dac/fig_intro.{pdf,png}, also paper/figs/fig_intro.pdf), single column, 2 x 2 panels:
(a) page mapping, (b) on-demand mapping, (c) block-level hybrid mapping, (d) DUET.
Each panel: on-device DRAM (dashed box = the mapping-DRAM budget) above NAND flash. NAND blocks are 8 pages in a
thick black frame (an IU = 4 pages in the drawing), and '...' marks that NAND has many more blocks. Colors:
pink = mapping (DRAM entries and map pages in NAND, always at the NAND top-right), purple = block- or IU-mapped data,
light blue = page-mapped data, white = free. An H-shaped bracket under each arrow shows the unit one map entry points to
(a page, a block, or an IU). Hatched pages are invalid. (c)/(d) show the merge copying only valid pages.
All outlines are black."""
import shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, FancyArrowPatch

OUT = '/home/sejun000/dual_mapping/results/paper_dac/fig_intro'
PAPER = '/home/sejun000/dual_mapping/paper/figs/fig_intro.pdf'
K = 'black'
C_BAD, C_OK, C_AR = '#B3261E', '#1D6E3A', '#222222'
F_MAP, F_CO, F_PG, F_FREE = '#F5C6D9', '#CFCBF0', '#C9E3F5', 'white'
F_INV = '#A6A6A6'   # (2026-11-13, user) invalid pages: darker gray fill instead of hatching
FS = 6.5
SF = FS - 0.7                # label size
plt.rcParams.update({'font.size': FS, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4})

YMIN, YMAX = 0.05, 6.38   # was 6.88 (2026-11-13: compact)
PW, PH = 0.3, 0.55           # page width / height in a block (block = 8 pages = 2.4 wide)
DX0, DX1, DY0, DY1 = 0.5, 9.7, 4.75, 6.22     # DRAM budget box
NX0, NX1, NY0, NY1 = 0.5, 9.7, 0.9, 4.08    # NAND box
EW, EH = 0.38, 0.5           # DRAM map-entry cell
EY = 5.0                    # map-entry row (bottom y)
ROW = [3.05, 1.75]            # NAND block rows (bottom y); top-left slot = map pages
COLX = [0.8, 3.55, 6.3]      # NAND block columns (left x)
HH = 0.26                    # H-bracket height


def frame(ax, title):
    ax.set_xlim(0, 10); ax.set_ylim(YMIN, YMAX); ax.axis('off')
    ax.text(5.1, 0.45, title, ha='center', va='center', fontsize=FS + 0.3)   # sub-caption below
    ax.add_patch(Rectangle((DX0, DY0), DX1 - DX0, DY1 - DY0, fc='none', ec=K, lw=0.6, ls=(0, (3, 2))))
    ax.add_patch(Rectangle((NX0, NY0), NX1 - NX0, NY1 - NY0, fc='#F7F7F5', ec=K, lw=0.6))
    ax.text(0.2, (DY0 + DY1) / 2, 'DRAM', rotation=90, ha='center', va='center', fontsize=SF - 0.4)
    ax.text(0.2, (NY0 + NY1) / 2, 'NAND', rotation=90, ha='center', va='center', fontsize=SF - 0.4)
    for y in ROW:                                        # NAND has many more blocks
        ax.text(9.2, y + PH / 2, '…', ha='center', va='center', fontsize=FS + 1, fontweight='bold')


def entries(ax, x, n, label):
    for i in range(n):
        ax.add_patch(Rectangle((x + i * EW, EY), EW, EH, fc=F_MAP, ec=K, lw=0.4))
    ax.text(x, EY + EH + 0.38, label, fontsize=SF, va='center')
    return lambda i: (x + (i + 0.5) * EW, EY)            # bottom-center of entry i


class Blk:
    """len(fills) pages (8 by default) at (x, y), each pw wide; fills[i] = face color; pages in `hatch` are invalid (gray);
    iu_split draws a chunk boundary every 4 pages."""
    def __init__(self, ax, x, y, fills, hatch=(), iu_split=False, pw=PW, ph=PH):
        self.ax, self.x, self.y, self.pw, self.ph, self.n = ax, x, y, pw, ph, len(fills)
        for i, fc in enumerate(fills):
            ax.add_patch(Rectangle((x + i * pw, y), pw, ph, fc=F_INV if i in hatch else fc, ec=K, lw=0.35))
        ax.add_patch(Rectangle((x, y), pw * self.n, ph, fc='none', ec=K, lw=1.0))
        if iu_split:
            for j in range(4, self.n, 4):
                ax.plot([x + j * pw] * 2, [y - 0.1, y + ph + 0.1], color=K, lw=1.0)

    @property
    def w(self):
        return self.pw * self.n

    def span(self, i, n=1):                              # x-range of pages [i, i+n)
        return self.x + i * self.pw, self.x + (i + n) * self.pw

    def bottom(self, i):
        return self.x + (i + 0.5) * self.pw, self.y

    def top(self, i):
        return self.x + (i + 0.5) * self.pw, self.y + self.ph + 0.03

    def label(self, s, color=K):
        lab(self.ax, self.x + self.w / 2, self.y - 0.27, s, color, ha='center')


def hbar(ax, x0, x1, y):
    """H-shaped bracket over [x0, x1] standing on y. Returns the crossbar midpoint."""
    yb, yt, ym = y + 0.03, y + 0.03 + HH, y + 0.03 + HH / 2
    ax.plot([x0, x0], [yb, yt], color=C_AR, lw=0.6)
    ax.plot([x1, x1], [yb, yt], color=C_AR, lw=0.6)
    ax.plot([x0, x1], [ym, ym], color=C_AR, lw=0.6)
    return (x0 + x1) / 2, ym


def point(ax, src, blk, i, n=1, color=C_AR, lw=0.6, rad=0.0):
    """arrow from a map entry to an H bracket over pages [i, i+n) of blk."""
    x0, x1 = blk.span(i, n)
    tip = hbar(ax, x0 + 0.03, x1 - 0.03, blk.y + blk.ph)
    arrow(ax, src, tip, color, lw=lw, rad=rad)


def arrow(ax, p, q, color=C_AR, rad=0.0, lw=0.6):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle='-|>', mutation_scale=4.5, color=color, lw=lw,
                                 connectionstyle=f'arc3,rad={rad}', shrinkA=0, shrinkB=0.3))


def badge(ax, x, y, s):
    ax.text(x, y, s, fontsize=SF - 0.5, color='white', ha='center', va='center', fontweight='bold',
            bbox=dict(boxstyle='circle,pad=0.12', fc=C_BAD, ec='none'))


def lab(ax, x, y, s, color=K, ha='left', **kw):
    kw.setdefault('fontsize', SF)
    ax.text(x, y, s, ha=ha, va='center', color=color, **kw)


def panel_page(ax):
    frame(ax, '(a) Page mapping')
    x0 = 0.45
    for r in range(4):                                   # full table, larger than the budget box
        for i in range(25):
            ax.add_patch(Rectangle((x0 + i * EW, 4.33 + r * 0.5), EW, 0.43, fc=F_MAP, ec=K, lw=0.3))
    ax.add_patch(Rectangle((DX0, DY0), DX1 - DX0, DY1 - DY0, fc='none', ec=K, lw=1.0, ls=(0, (3, 2))))
    ax.text(5.1, 5.5, 'page-level map\nexceeds DRAM capacity', ha='center', va='center', fontsize=SF,
            fontweight='bold', linespacing=1.0, bbox=dict(fc='white', ec=K, lw=0.4, pad=1.0))   # the dashed box
    b = [Blk(ax, COLX[c], ROW[r], [F_PG] * 8) for r in range(2) for c in range(3)]
    for e, (j, i) in zip((3, 11, 19), ((0, 2), (1, 5), (2, 3))):
        point(ax, (x0 + (e + 0.5) * EW, 4.33), b[j], i)


def panel_ondemand(ax):
    frame(ax, '(b) On-demand mapping')
    ce = entries(ax, 0.9, 16, 'map cache')
    d1 = Blk(ax, COLX[0], ROW[0], [F_PG] * 8)
    d2 = Blk(ax, COLX[1], ROW[0], [F_PG] * 8)
    tp = Blk(ax, COLX[2], ROW[0], [F_MAP] * 8)
    tp.label('map pages')
    Blk(ax, COLX[0], ROW[1], [F_PG] * 8)
    d3 = Blk(ax, COLX[1], ROW[1], [F_PG] * 8)
    Blk(ax, COLX[2], ROW[1], [F_PG] * 8)
    point(ax, ce(2), d1, 3)                              # hit: entry -> one page
    point(ax, ce(8), d2, 4)
    arrow(ax, ce(14), tp.top(3), C_BAD, lw=0.9)          # miss: (1) map page, then (2) data page
    badge(ax, 6.7, 4.42, '1')
    lab(ax, 5.05, 4.42, 'miss', C_BAD, fontweight='bold')
    arrow(ax, (tp.x, tp.y + PH / 2), d3.top(5), C_BAD, lw=0.9, rad=0.25)
    badge(ax, 5.45, 2.95, '2')


def panel_hybrid(ax):
    frame(ax, '(c) Block-level hybrid mapping')
    be = entries(ax, 0.9, 6, 'block map')
    le = entries(ax, 5.55, 6, 'page-level map')
    A = Blk(ax, COLX[0], ROW[0], [F_CO] * 8, hatch=(2, 4, 5))
    B = Blk(ax, COLX[1], ROW[0], [F_CO] * 8, hatch=(1,))
    L = Blk(ax, COLX[2], ROW[0], [F_PG] * 5 + [F_FREE] * 3, hatch=(3,))
    lab(ax, (A.x + B.x + 8 * PW) / 2, A.y - 0.27, 'data blocks', ha='center')
    L.label('log block')
    point(ax, be(1), A, 0, 8)                            # one entry -> a whole block
    point(ax, be(4), B, 0, 8)
    point(ax, le(1), L, 1)                               # one entry -> one page
    point(ax, le(4), L, 4)
    # merge: valid pages of data block A and of the log block -> a new block; both are erased
    N = Blk(ax, COLX[0], ROW[1], [F_CO, F_CO, F_PG, F_CO, F_PG, F_PG, F_CO, F_CO])
    N.label('new block')
    arrow(ax, A.bottom(1), N.top(1), C_BAD, lw=0.9)
    arrow(ax, L.bottom(1), N.top(5), C_BAD, lw=0.9, rad=-0.1)
    lab(ax, 3.55, 1.95, 'block-level merge', C_BAD, fontweight='bold')


def panel_duet(ax):
    frame(ax, '(d) DUET')
    ie = entries(ax, 0.9, 6, 'coarse IU map')
    pe = entries(ax, 5.3, 6, 'page-level map')
    # IU-mapped block: two IUs of 4 pages; pages 1-2 of the first IU are superseded by 4 KiB pages
    I = Blk(ax, COLX[0], ROW[0], [F_CO] * 8, hatch=(1, 2), iu_split=True)
    P = Blk(ax, COLX[1], ROW[0], [F_PG] * 6 + [F_FREE] * 2, hatch=(4,))
    S = Blk(ax, COLX[2], ROW[0], [F_FREE] * 3 + [F_MAP] * 2 + [F_FREE] * 3)
    S.label('map pages')
    M = Blk(ax, COLX[0], ROW[1], [F_CO, F_PG, F_PG, F_CO] + [F_FREE] * 4, iu_split=True)
    lab(ax, M.x, 1.27, 'coarse-IU\nmerge', C_OK, fontsize=SF - 0.6, fontweight='bold', linespacing=0.95)
    Blk(ax, COLX[1], ROW[1], [F_PG] * 3 + [F_FREE] * 5)
    Blk(ax, COLX[2], ROW[1], [F_CO] * 8, iu_split=True)
    ax.add_patch(Rectangle((COLX[1] - 0.12, ROW[1] - 0.12), 8 * PW + 0.24, ROW[0] + PH - ROW[1] + 0.24,
                           fc='none', ec=K, lw=0.6, ls=(0, (2, 1.5))))
    lab(ax, COLX[1] + 4 * PW + 0.3, ROW[1] - 0.55, 'page-level', ha='center', fontsize=SF - 0.6)
    point(ax, ie(1), I, 0, 4)                            # one entry -> one IU
    point(ax, ie(4), I, 4, 4)
    point(ax, pe(0), P, 1)                               # one entry -> one page
    point(ax, pe(2), P, 5)
    # a few page-level mappings are staged in map pages and promoted back on access
    ax.add_patch(FancyArrowPatch(pe(5), S.top(4), arrowstyle='<|-|>', mutation_scale=4.5, color=C_AR, lw=0.6,
                                 ls=(0, (2, 1.2)), shrinkA=0, shrinkB=0.3))
    lab(ax, 7.75, 4.42, 'spill/load', fontsize=SF - 1.2)   # one line: fits the narrower DRAM-NAND gap (2026-11-13)
    # merge one IU: its 2 valid pages + its 2 page-level pages -> a new IU
    arrow(ax, I.bottom(0), M.top(0), C_OK, lw=0.9)
    arrow(ax, P.bottom(1), M.top(2), C_OK, lw=0.9, rad=-0.1)


def main():
    fig, axs = plt.subplots(2, 2, figsize=(3.4, 2.19))   # was 2.36 (2026-11-13)
    for ax, f in zip(axs.flat, (panel_page, panel_ondemand, panel_hybrid, panel_duet)):
        f(ax)
    fig.subplots_adjust(left=0.0, right=1.0, top=1.0, bottom=0.0, wspace=0.02, hspace=0.035)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}.{ext}', dpi=300, bbox_inches='tight', pad_inches=0.01)
    shutil.copy(OUT + '.pdf', PAPER)
    print('wrote', OUT + '.{pdf,png}', 'and', PAPER)


if __name__ == '__main__':
    main()
