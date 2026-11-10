#!/usr/bin/env python3
"""Fine-area size over time for DUET (results/final_ts, q_ts.sh: [dm flush] dumped every 5 s).
Left axis: number of fine blocks. Right axis: fine pages (= fine-map entries), split into entries in the DRAM table
(fpages - spages) and spilled entries (spages). Blue line: all fine-map entries. Table capacity (mem = NIU*4 + GTD + slots*8) printed only, not drawn.
Single column, two panels. Outputs results/paper_dac/fig_ts.{pdf,png} (also copied to paper/figs)."""
import re, shutil
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

B = '/home/sejun000/dual_mapping/results'
OUT = B + '/paper_dac'
WL = [('apps/ycsba_d_t64', 'YCSB-A'), ('trace_ali3', 'Alibaba')]   # R:W 50:50 dropped (flat, no spill)
C_DUET, C_SP, C_FB, C_TOT = '#9A91E6', '#E2DFF8', '#C0392B', '#1F3FBF'
YMAX = 5.0   # % of logical pages
plt.rcParams.update({'font.size': 7, 'font.family': 'DejaVu Sans', 'axes.linewidth': 0.6})


def load(d):
    rows = []
    for l in open(f'{B}/final_ts/{d}/ts.log'):
        kv = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', l)}
        if 'fpages' in kv:
            rows.append((int(l.split()[0]), kv))
    t0 = rows[0][0]
    L = rows[0][1]['L']; niu = L // 16; gtd = -(-niu // 64) * 4
    cap = (rows[0][1]['mem'] - niu * 4 - gtd) // 8
    return [t - t0 for t, _ in rows], rows, cap


def main():
    fig, axs = plt.subplots(1, 2, figsize=(3.4, 1.25))   # was 1.45 (2026-11-09)
    for ax, (d, lab) in zip(axs, WL):
        t, rows, cap = load(d)
        fb = [r['fine_blocks'] for _, r in rows]
        L = rows[0][1]['L']   # fine-map entries as % of all logical pages (2026-11-08)
        tab = [(r['fpages'] - r['spages']) * 100 / L for _, r in rows]
        sp = [r['spages'] * 100 / L for _, r in rows]
        ax.stackplot(t, tab, sp, colors=[C_DUET, C_SP], lw=0, alpha=0.9)
        ax.plot(t, [a + b for a, b in zip(tab, sp)], color=C_TOT, lw=0.8)
        ax.set_ylim(0, YMAX)
        ax.set_xlim(0, t[-1]); ax.set_xlabel('Time (s)', fontsize=6.5, labelpad=1)
        ax.set_title(lab, fontsize=7, pad=2)
        ax.tick_params(labelsize=6, pad=1, length=2); ax.tick_params(axis='y', colors=C_TOT)
        if ax is axs[0]:
            ax.set_ylabel('Fine-map entries (%)', fontsize=6.5, labelpad=1.5, color=C_TOT)
        a2 = ax.twinx()
        a2.plot(t, fb, color=C_FB, lw=0.9)
        a2.set_ylim(0, 20); a2.tick_params(labelsize=6, pad=1, length=2, colors=C_FB)
        if ax is axs[-1]:
            a2.set_ylabel('Fine blocks', fontsize=6.5, color=C_FB, labelpad=1.5)
        print(lab, 'cap', cap, 'max table', max(tab), 'max spilled', max(sp), 'max fb', max(fb), 'samples', len(t))
    from matplotlib.patches import Patch
    from matplotlib.lines import Line2D
    h = [Line2D([], [], color=C_TOT, lw=0.8, label='Fine-map entries'), Patch(fc=C_DUET, label='In DRAM table'),
         Patch(fc=C_SP, label='Spilled'),
         Line2D([], [], color=C_FB, lw=0.9, label='Fine blocks')]
    fig.tight_layout(pad=0.2, w_pad=0.6); fig.subplots_adjust(top=0.78)   # was 0.72 (2026-11-10)
    fig.legend(handles=h, ncol=4, fontsize=6, loc='upper center', bbox_to_anchor=(0.5, 1.03), frameon=False,
               handlelength=1.4, columnspacing=0.9, handletextpad=0.4)
    for ext in ('pdf', 'png'):
        fig.savefig(f'{OUT}/fig_ts.{ext}', dpi=250, bbox_inches='tight', pad_inches=0.02)
    shutil.copy(f'{OUT}/fig_ts.pdf', '/home/sejun000/dual_mapping/paper/figs/fig_ts.pdf')
    print('wrote', OUT + '/fig_ts.{pdf,png} and paper/figs')


if __name__ == '__main__':
    main()
