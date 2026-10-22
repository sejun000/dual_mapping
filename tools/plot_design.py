#!/usr/bin/env python3
"""DUET overview diagram (results/paper_dac/fig_design.{pdf,png}), single column.
DRAM (1/8 budget): coarse map (4 B per 64 KiB IU), fine index (cuckoo hash), GTD (4 B per FTP).
NAND: coarse blocks of 64 KiB chunks, fine blocks of 4 KiB fragments, FTPs (fragment pointers of 64 IUs).
Solid arrows: lookups; dashed: spill / clean promotion; grey: fold / refold; host writes on the right."""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, Rectangle, FancyArrowPatch

OUT = '/home/sejun000/dual_mapping/results/paper_dac/fig_design'
C_CO, C_FI, C_MP, C_GR = '#534AB7', '#D85A30', '#1D9E75', '#444441'
L_CO, L_FI, L_MP = '#DCD9F2', '#F6D9CE', '#CFEDE3'
FS = 5.6
plt.rcParams.update({'font.size': FS, 'font.family': 'DejaVu Sans'})


def panel(ax, x, y, w, h, title):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle='round,pad=0.02,rounding_size=0.15', fc='#F7F7F5', ec='#888780', lw=0.7))
    ax.text(x + w / 2, y + h - 0.3, title, ha='center', va='center', fontsize=FS + 0.4, fontweight='bold')


def box(ax, x, y, w, h, fc, ec, lw=0.6):
    ax.add_patch(Rectangle((x, y), w, h, fc=fc, ec=ec, lw=lw))


def arrow(ax, p, q, color, ls='-', rad=0.0, lw=0.9):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle='-|>', mutation_scale=6, color=color, lw=lw, ls=ls,
                                 connectionstyle=f'arc3,rad={rad}', shrinkA=0, shrinkB=0))


def label(ax, x, y, s, color, **kw):
    ax.text(x, y, s, ha='left', va='center', fontsize=FS, color=color, fontweight='bold', **kw)


def main():
    fig, ax = plt.subplots(figsize=(3.4, 2.45))
    ax.set_xlim(0, 13.6); ax.set_ylim(0, 8.0); ax.axis('off')
    panel(ax, 0.05, 0.1, 5.0, 7.8, 'DRAM (1/8 budget)')
    panel(ax, 6.45, 0.1, 5.0, 7.8, 'NAND flash')
    X0, X1 = 0.35, 6.75            # left edge of the DRAM / NAND contents
    W = 4.4                         # content width

    # --- coarse level (top row)
    label(ax, X0, 6.95, 'Coarse map: 4 B per IU', C_CO)
    for i in range(6):
        box(ax, X0 + i * W / 6, 5.75, W / 6 - 0.06, 0.8, L_CO, C_CO)
    ax.text(X0 + W / 12, 6.15, 'F S\nchunk', ha='center', va='center', fontsize=4.3, linespacing=0.9)
    label(ax, X1, 6.95, 'Coarse blocks: 64K chunks', C_CO)
    for c in range(4):
        box(ax, X1 + c * W / 4, 5.75, W / 4 - 0.08, 0.8, L_CO, C_CO)
        for k in range(1, 4):
            xx = X1 + c * W / 4 + k * (W / 4 - 0.08) / 4
            ax.plot([xx, xx], [5.75, 6.55], color=C_CO, lw=0.3)
    arrow(ax, (X0 + W, 6.15), (X1, 6.15), C_CO)

    # --- fine level (middle row)
    label(ax, X0, 5.2, 'Fine map: cuckoo hash', C_FI)
    for b in range(6):
        for w in range(4):
            box(ax, X0 + b * W / 6, 2.85 + w * 0.52, W / 6 - 0.06, 0.46, L_FI if b in (1, 4) else 'white', C_FI, lw=0.5)
    label(ax, X1, 5.2, 'Fine blocks: 4K fragments', C_FI)
    for r in range(4):
        for c in range(12):
            box(ax, X1 + c * W / 12, 2.85 + r * 0.52, W / 12 - 0.05, 0.46, L_FI if (r * 7 + c * 3) % 5 else 'white', C_FI, lw=0.4)
    arrow(ax, (X0 + W, 3.9), (X1, 3.9), C_FI)

    # --- map pages (bottom row)
    label(ax, X0, 1.85, 'GTD: 4 B per FTP', C_MP)
    for i in range(6):
        box(ax, X0 + i * W / 6, 0.45, W / 6 - 0.06, 0.8, L_MP, C_MP)
    label(ax, X1, 1.85, 'FTPs: pointers of 64 IUs', C_MP)
    for i in range(4):
        box(ax, X1 + i * W / 4, 0.45, W / 4 - 0.08, 0.8, L_MP, C_MP)
    arrow(ax, (X0 + W, 0.85), (X1, 0.85), C_MP)

    # --- spill (fine index -> FTP) and clean promotion (FTP -> fine index), through the gap
    arrow(ax, (X0 + W - 0.2, 2.85), (X1 + 0.25, 1.25), C_MP, ls='--')
    arrow(ax, (X1 + 0.65, 1.25), (X0 + W + 0.25, 2.85), C_MP, ls=(0, (1, 1)))
    ax.text(5.75, 2.35, 'spill', fontsize=4.8, color=C_MP, ha='center', va='center', rotation=0,
            bbox=dict(fc='white', ec='none', pad=0.3))
    ax.text(5.75, 1.75, 'promote\n(clean)', fontsize=4.4, color=C_MP, ha='center', va='center', linespacing=0.9,
            bbox=dict(fc='white', ec='none', pad=0.2))

    # --- fold / refold: fragments + chunk -> new chunk (right edge of NAND)
    arrow(ax, (X1 + W - 0.1, 4.95), (X1 + W - 0.1, 5.7), C_GR, rad=0.9, lw=0.8)
    ax.text(12.05, 5.0, 'fold /\nrefold\n(1 IU)', fontsize=4.6, color=C_GR, ha='left', va='center', linespacing=0.9)

    # --- host writes (from the far right)
    ax.text(12.9, 7.55, 'Host\nwrite', fontsize=FS, ha='center', va='center', fontweight='bold', linespacing=0.9)
    arrow(ax, (12.9, 7.05), (X1 + W + 0.15, 6.4), C_CO, rad=0.2)
    ax.text(12.75, 6.45, 'full IU', fontsize=4.6, color=C_CO, ha='left', va='center')
    arrow(ax, (13.2, 7.05), (X1 + W + 0.15, 3.3), C_FI, rad=-0.15)
    ax.text(12.45, 2.9, 'partial\n(no RMW)', fontsize=4.6, color=C_FI, ha='left', va='center', linespacing=0.9)

    fig.tight_layout(pad=0.05)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}.{ext}', dpi=300, bbox_inches='tight', pad_inches=0.02)
    print('wrote', OUT + '.{pdf,png}')


if __name__ == '__main__':
    main()
