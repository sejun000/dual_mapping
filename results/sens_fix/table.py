import json, sys
import os; SP=os.path.dirname(os.path.abspath(__file__)) + '/..'
WLS=['z0','z0.8','z0.99','z1.2','ali1','ali2','ali3','ssd','kv_w1','spfresh','ycsbb_d_t64','ycsba_d_t64','ycsbfstd_d_t64','tpcc_d_t64','fio_randwrite_d','fileserverfs']
BS=[1000,1250,1875,2500,3750]; VS=['base','of','ofpc','ofpcu']
def W(wl,b,v):
    try: return json.load(open(f'{SP}/sens_fix/{wl}/prop_U16_b{b}_{v}.json'))['WAF']
    except Exception: return float('nan')
out=[]
out.append('WAF, proposed V3 U=16, OP 7%; budgets = mapping DRAM as % of the 4 B page map')
out.append('base = current default; of = onefront_fix=1; ofpc = onefront_fix=1 pool_cap=1; ofpcu = onefront_fix=1 pool_cap=4 (cap in uniform mode only)')
for v in VS:
    out.append(f'\n[{v}]' + ''.join(f'{b/100:>9.2f}%' for b in BS) + '   monotone?   d18.75 vs base')
    for wl in WLS:
        r=[W(wl,b,v) for b in BS]
        bad=[f'{BS[i]/100:g}->{BS[i+1]/100:g}' for i in range(4) if r[i+1] > r[i]*1.005]
        d=(r[2]/W(wl,1875,'base')-1)*100
        out.append(f'{wl:16s}'+''.join(f'{x:10.3f}' for x in r)+f'   {",".join(bad) or "yes":24s} {d:+.2f}%')
print('\n'.join(out))
