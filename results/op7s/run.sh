#!/bin/bash
# ftlsim on the spatially sampled traces that NVMeVirt replays (results/nvmevirt/queue_trace.sh), same configs as op7b
# (tools/run_op7.sh: OP 7%, V3 proposed with 18.75%/10% map budget, DFTL/LeaFTL/mu-FTL with the same byte budgets,
# page4k, SepBIT, FAST, IU+RMW). Traces: /mnt/nvme_mounted/dm_trace_sampled/<t>.csv (trace_prep sample=k/1000
# order=touch reads=1; ftlsim ignores the reads). CPUs: even cores 0-92 (94,95 belong to NVMeVirt).
cd /home/sejun000/dual_mapping
D=/mnt/nvme_mounted/dm_trace_sampled
for t in ${TRACES:-spfresh kv_w1 ali1 ali3 ali2 ssd}; do
  CAP=$(python3 -c "import json;print(json.load(open('$D/$t.json'))['capacity_bytes'])")
  read FASTCAP FASTP < <(python3 -c "
c=$CAP; L=c//4096; p=L*3//1024; p-=p%1024; p=max(p,1024); print(-(-L//p)*p*4096, p)")
  FASTCAP=$FASTCAP FASTP=$FASTP taskset -c 0-92:2 tools/run_op7.sh results/op7s/$t trace=$D/$t.csv capacity=$CAP &
done
wait
python3 - <<'PY'
import json, os
pols = [('V3_18.75%', 'P12'), ('page4k', 'page4k'), ('DFTL', 'dftl12'), ('LeaFTL', 'leaftl12'), ('FAST', 'fast'), ('uFTL', 'mu12'), ('IU+RMW', 'iu_rmw'), ('V3_10%', 'P10'), ('SepBIT', 'sepbit')]
orig = {'P12': 'V3_18'}
out = []
out.append('WAF, sampled trace (this dir) vs full trace (results/op7b); OP 7%, 18.75% map DRAM budget')
out.append(f"{'trace':8s} {'':6s} " + ' '.join(f'{n:>10s}' for n, _ in pols))
for t in ['spfresh', 'kv_w1', 'ali1', 'ali3', 'ali2', 'ssd']:
    for lab, root, m in (('sample', 'results/op7s', {}), ('full', 'results/op7b', orig)):
        row = []
        for _, f in pols:
            p = f'{root}/{t}/{m.get(f, f)}.json'
            try: row.append(f"{json.load(open(p))['WAF']:10.3f}")
            except Exception: row.append(f"{'-':>10s}")
        out.append(f'{t:8s} {lab:6s} ' + ' '.join(row))
open('results/op7s/SUMMARY.txt', 'w').write('\n'.join(out) + '\n')
print('\n'.join(out))
PY
