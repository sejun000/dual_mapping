#!/usr/bin/env python3
"""DUET write and reclamation paths (results/paper_dac/fig_write.{pdf,png}, also paper/figs/fig_write.pdf), single
column. Same drawing rules as the intro figure (tools/plot_intro.py): blocks of 16 pages, a chunk = 4 pages (U = 4 in
the drawing), purple = chunks, blue = fine pages, white = free, hatched = invalid.
Coarse column: active host / GC blocks, the coarse GC victim, a coarse block. Fine column: active host / GC blocks, the
fine GC victim, a fine block. Coarse GC (purple badges): (1) an all-valid chunk is copied, (2) a partly valid chunk
(LPN 20-23) is merged with its fine pages (LPN 21, 23, found in the fine map), an invalid chunk is skipped. Fine GC
(blue badges): (1) live fine pages are copied, (2) the fine pages of several coarse IUs (LPN 9, 11 / 25) are merged
with their chunks (LPN 8-11, 24-27), one new chunk per coarse IU."""
import shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Rectangle
from plot_intro import Blk, arrow, lab, K, C_AR, C_BAD, F_CO, F_PG, F_FREE, FS, SF

OUT = '/home/sejun000/dual_mapping/results/paper_dac/fig_write'
PAPER = '/home/sejun000/dual_mapping/paper/figs/fig_write.pdf'
plt.rcParams.update({'font.size': FS, 'font.family': 'DejaVu Sans', 'hatch.linewidth': 0.4})

XMAX, YMAX = 19.2, 11.65   # was 20.0 with CX 4.5 (2026-11-10: less left margin)
HY = 10.7                        # y of the two column headers (legend row above it)
BP, PH = 0.34, 0.7               # page width / height: 16-page blocks (5.44 wide), a chunk = 4 pages
CX, FX = 3.7, 13.5               # coarse / fine column
RA, RB, RC, RD = 8.5, 6.8, 4.2, 1.55  # rows: active host, active GC, GC victims, blocks read by merges
C_CG, C_FG = '#5B3FA0', '#1F5FA8'     # badge colors: coarse GC, fine GC


def line(ax, p, q, rad=0.0, color=C_AR):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle='-', color=color, lw=0.6, connectionstyle=f'arc3,rad={rad}',
                                 shrinkA=0, shrinkB=0))


def badge(ax, x, y, s, c):
    ax.text(x, y, s, fontsize=SF - 0.9, color='white', ha='center', va='center', fontweight='bold',
            bbox=dict(boxstyle='circle,pad=0.1', fc=c, ec='none'))


def blk(ax, x, y, fills, **kw):
    return Blk(ax, x, y, fills, pw=BP, ph=PH, **kw)


def cx(b, c):                    # x center of chunk c of block b
    return b.x + (4 * c + 2) * BP


def main():
    fig, ax = plt.subplots(figsize=(3.4, 3.4 * YMAX / XMAX))
    ax.set_xlim(0, XMAX); ax.set_ylim(0, YMAX); ax.axis('off')
    sm = dict(fontsize=SF - 0.3)     # was SF - 0.7 (2026-11-10)
    tiny = dict(fontsize=SF - 0.7)   # was SF - 1.1 (2026-11-10)

    # ---- active blocks
    ch = blk(ax, CX, RA, [F_CO] * 4 + [F_FREE] * 12, iu_split=True)
    gc = blk(ax, CX, RB, [F_CO] * 16, iu_split=True)       # chunks: copy 0-3, merged 20-23, merged 8-11, merged 24-27
    fh = blk(ax, FX, RA, [F_PG] * 5 + [F_FREE] * 11)
    fg = blk(ax, FX, RB, [F_PG] * 8 + [F_FREE] * 8)          # the 8 fine pages copied by fine GC (1)
    for b in (ch, fh):                                   # one box per pair of active blocks (host + GC)
        ax.add_patch(Rectangle((b.x - 0.25, RB - 0.3), b.w + 0.5, RA + PH + 1.0 - (RB - 0.3), fc='#F7F7F5', ec=K,
                               lw=0.6, zorder=0))
    for b, s in ((ch, 'host'), (gc, 'internal'), (fh, 'host'), (fg, 'internal')):
        lab(ax, b.x, b.y + PH + 0.62, s, **sm)
    lab(ax, CX + ch.w / 2, HY, 'active coarse blocks', ha='center', fontweight='bold')
    lab(ax, FX + fh.w / 2, HY, 'active fine blocks', ha='center', fontweight='bold')

    # ---- legend: one horizontal row above the column headers, line colors of the two kinds of GC
    # centered at the midpoint of the two column headers
    LW, LG, GAP, ly = 0.8, 0.15, 1.0, YMAX - 0.2
    rend = fig.canvas.get_renderer()
    items = []
    for c, t in ((C_CG, 'coarse GC'), (C_FG, 'fine GC')):
        txt = ax.text(0, ly, t, color=c, fontweight='bold', va='center', **sm)
        bb = txt.get_window_extent(renderer=rend).transformed(ax.transData.inverted())
        items.append((c, txt, bb.width))
    total = sum(LW + LG + w for _, _, w in items) + GAP * (len(items) - 1)
    x = (CX + ch.w / 2 + FX + fh.w / 2) / 2 - total / 2
    for c, txt, w in items:
        ax.plot([x, x + LW], [ly, ly], color=c, lw=1.0)
        txt.set_x(x + LW + LG)
        x += LW + LG + w + GAP

    # ---- host writes
    lab(ax, CX - 1.6, RA + PH / 2, 'IU-aligned\nwrite', ha='right', multialignment='center', linespacing=0.95, **sm)
    arrow(ax, (CX - 1.45, RA + PH / 2), (CX - 0.05, RA + PH / 2))
    arrow(ax, (CX + ch.w + 0.75, RA + PH / 2), (FX - 0.05, RA + PH / 2))   # starts clear of the coarse box
    lab(ax, (CX + ch.w + FX) / 2 + 0.15, RA - 0.3, 'partial write\n(< IU,\nunaligned)', ha='center', linespacing=0.95,
        **sm)

    # ---- coarse GC victim: 0-3 all valid, 20-23 partly valid (21, 23 invalid, newer data in fine pages), the rest invalid
    cv = blk(ax, CX, RC, [F_CO] * 16, hatch=(5, 7) + tuple(range(8, 16)), iu_split=True)
    lab(ax, cv.x + cv.w / 2, RC - 0.8, 'victim coarse block', ha='center', fontweight='bold', **sm)
    lab(ax, CX - 0.2, RC - 0.3, 'LPN', ha='right', **tiny)
    for c, s in ((0, '0–3'), (1, '20–23')):
        lab(ax, cx(cv, c), RC - 0.3, s, ha='center', **tiny)
    arrow(ax, (cx(cv, 0), RC + PH + 0.03), (cx(gc, 0), RB - 0.03), C_CG)            # (1) copy
    badge(ax, cx(cv, 0) - 0.35, (RC + PH + RB) / 2, '1', C_CG)
    # fine block below it with the fine pages of LPN 21 and 23 (fine map lookup)
    fb = blk(ax, CX, RD, [F_PG] * 12 + [F_FREE] * 4, hatch=(1, 5, 6, 10))
    for k, s in ((3, '21'), (8, '23')):
        lab(ax, fb.x + (k + 0.5) * BP, RD - 0.3, s, ha='center', **tiny)
        ax.add_patch(Rectangle((fb.x + k * BP + 0.03, RD + 0.03), BP - 0.06, PH - 0.06, fc='none', ec=C_CG, lw=1.0))
        ax.plot([fb.x + (k + 0.5) * BP] * 2, [RD + PH, RD + PH + 0.3], color=C_CG, lw=0.5)
    ax.plot([fb.x + 3.5 * BP, fb.x + 8.5 * BP], [RD + PH + 0.3] * 2, color=C_CG, lw=0.5)
    lab(ax, fb.x + 6 * BP, RD + PH + 0.6, 'invalid after merge', ha='center', color=C_CG, **tiny)
    lab(ax, fb.x + fb.w / 2, RD - 0.8, 'fine block', ha='center', fontweight='bold', **sm)
    # (2) merge: chunk 20-23 and its fine pages join, one new chunk
    jx, jy, rx = cx(cv, 1), RC + PH + 0.7, CX + cv.w + 0.45
    ax.plot([fb.x + fb.w, rx, rx, jx], [RD + PH / 2, RD + PH / 2, jy, jy], color=C_CG, lw=0.6)
    ax.plot([jx, jx], [RC + PH + 0.03, jy], color=C_CG, lw=0.6)
    ax.plot(jx, jy, 'o', ms=1.6, color=C_CG)
    lab(ax, jx + 0.15, (RC + PH + jy) / 2, '20, 22', color=C_CG, **tiny)            # what each source sends
    lab(ax, rx + 0.15, RD + PH + 0.5, '21, 23', color=C_CG, **tiny)
    arrow(ax, (jx, jy), (cx(gc, 1), RB - 0.03), C_CG)
    badge(ax, rx, (RD + RC) / 2 + 0.5, '2', C_CG)

    # ---- fine GC victim: (1) live fine pages copied, (2) fine pages of two coarse IUs merged with their chunks
    fv = blk(ax, FX, RC, [F_PG] * 16, hatch=(1, 3, 6, 10, 13))
    for k, s in ((2, '9'), (4, '11'), (8, '25')):                 # merged by (2)
        lab(ax, fv.x + (k + 0.5) * BP, RC - 0.3, s, ha='center', **tiny)
        ax.add_patch(Rectangle((fv.x + k * BP + 0.03, RC + 0.03), BP - 0.06, PH - 0.06, fc='none', ec=C_FG, lw=1.0))
    lab(ax, fv.x + fv.w / 2, RC - 0.8, 'victim fine block', ha='center', fontweight='bold', **sm)
    by = RC + PH + 0.35                                            # (1) copy: only the valid pages, gathered
    live = (0, 5, 7, 9, 11, 12, 14, 15)
    for k in live:
        ax.plot([fv.x + (k + 0.5) * BP] * 2, [RC + PH + 0.03, by], color=C_FG, lw=0.5)
    ax.plot([fv.x + 0.5 * BP, fv.x + 15.5 * BP], [by, by], color=C_FG, lw=0.6)
    arrow(ax, (fg.x + 4 * BP, by), (fg.x + 4 * BP, RB - 0.03), C_FG)
    badge(ax, fg.x + 4 * BP + 0.4, (by + RB) / 2, '1', C_FG)
    # coarse block below it: chunks 8-11 and 24-27, pages 9, 11, 25 invalid (newer data in fine pages)
    cb = blk(ax, FX, RD, [F_CO] * 16, hatch=(4, 5, 6, 7, 9, 11, 13), iu_split=True)
    for c, s in ((2, '8–11'), (3, '24–27')):
        lab(ax, cx(cb, c), RD - 0.3, s, ha='center', **tiny)
    lab(ax, cb.x + cb.w / 2, RD - 0.8, 'coarse block', ha='center', fontweight='bold', **sm)
    # (2) merge: the fine pages and the chunks join, one new chunk per coarse IU
    lx, ty = FX - 2.4, RC + PH + 1.2
    ax.plot([FX - 0.03, lx, lx], [RD + PH / 2, RD + PH / 2, ty], color=C_FG, lw=0.6)
    ax.plot([FX - 0.03, lx], [RC + PH / 2, RC + PH / 2], color=C_FG, lw=0.6)
    lab(ax, (FX + lx) / 2, RC + PH / 2 + 0.25, '9, 11, 25', ha='center', color=C_FG, **tiny)
    lab(ax, (FX + lx) / 2, RD + PH / 2 + 0.5, '8, 10,\n24, 26, 27', ha='center', color=C_FG, linespacing=0.95, **tiny)
    ax.plot([lx, cx(gc, 2)], [ty, ty], color=C_FG, lw=0.6)
    ax.plot(lx, RC + PH / 2, 'o', ms=1.6, color=C_FG)
    for c in (2, 3):
        arrow(ax, (cx(gc, c), ty), (cx(gc, c), RB - 0.03), C_FG)
    badge(ax, lx, (RD + RC) / 2 + 0.5, '2', C_FG)
    # new chunks in the active coarse GC block
    for c, s in enumerate(('0–3', '20–23', '8–11', '24–27')):
        lab(ax, cx(gc, c), RB + PH + 0.2, s, ha='center', **tiny)

    fig.subplots_adjust(left=0.0, right=1.0, top=1.0, bottom=0.0)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}.{ext}', dpi=300, bbox_inches='tight', pad_inches=0.01)
    shutil.copy(OUT + '.pdf', PAPER)
    print('wrote', OUT + '.{pdf,png}', 'and', PAPER)


if __name__ == '__main__':
    main()
