import sys
sys.path.insert(0, '/home/sejun000/dual_mapping/tools')
import sens
sens.SYN['z0.8'] = 'zipf=0.8'
OUT = sys.argv[1]
WLS = ['z0', 'z0.8', 'z0.99', 'z1.2', 'ali1', 'ali2', 'ali3', 'ssd', 'kv_w1', 'spfresh'] + sens.APPS
VAR = {'base': '', 'of': ' onefront_fix=1', 'ofpc': ' onefront_fix=1 pool_cap=1', 'ofpcu': ' onefront_fix=1 pool_cap=4'}
jobs = []
for wl in WLS:
    for b in (1000, 1250, 1875, 2500, 3750):
        w, tag, args = sens.job(wl, 'prop', b=b)
        for v, extra in VAR.items():
            jobs.append((sens.est(wl, tag), f'{OUT}/{wl} {tag}_{v} {args}{extra}'))
for _, j in sorted(jobs, key=lambda x: -x[0]): print(j)
