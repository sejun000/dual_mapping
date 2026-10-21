#!/bin/bash
# ftlsim on the sampled traces with the exact NVMeVirt device geometry of each trace (the check for the NVMeVirt runs):
# L = device logical pages (tools/nvmev_geom.py; includes the <1% slack beyond the trace's regions, which the seq fill
# writes and the trace never touches), P = device pages per line, NB = 373 lines (op chosen so floor(L(1+op)/P) = 373),
# FAST: L = L - L % P as the NVMeVirt FAST core. DFTL/LeaFTL/mu cache = 18.75% of L x 4 B (B18 of nvmev_cycle.sh).
# Output: results/op7s_dev/<trace>/<policy>.json, summary results/op7s_dev/SUMMARY.txt.
cd /home/sejun000/dual_mapping
D=/mnt/nvme_mounted/dm_trace_sampled; S=./ftlsim/ftlsim
V3="mode=proposed U=16 fine_extra=0 fine_quota=0.0429 fine_full=1 fine_policy=cost_cold gc_select=cost cgc_vict=cb coarse_class=2 adapt=2 uni_onefront=1 fine_index=cuckoo2 rec_block=1 map_budget=0.1875"
for t in ${TRACES:-spfresh kv_w1 ali1 ali3 ali2 ssd}; do
  O=results/op7s_dev/$t; mkdir -p $O
  CAP=$(python3 -c "import json;print(json.load(open('$D/$t.json'))['capacity_bytes'])")
  M=$(python3 tools/nvmev_geom.py --fit $CAP)
  read L P LF OP OPF < <(python3 -c "
import sys;sys.path.insert(0,'tools');from nvmev_geom import geom,parse;g=geom(parse('$M'))
L,P,LF=g['L'],g['P'],g['L_fast']
print(L,P,LF,'%.12f'%((373*P+0.5)/L-1),'%.12f'%((373*P+0.5)/LF-1))")
  B=$((L * 4 * 1875 / 10000)); W="trace=$D/$t.csv capacity=$((L * 4096)) P=$P"
  echo "$t memmap=$M L=$L P=$P op=$OP B18=$B"
  run() { local tag=$1; shift; taskset -c 0-92:2 $S "$@" tag=$tag > $O/$tag.json 2> $O/$tag.log; }
  run P12 $W $V3 op=$OP &
  run page4k $W mode=page4k op=$OP &
  run iu_rmw $W mode=iu_rmw op=$OP &
  run dftl12 $W mode=dftl op=$OP cache_bytes=$B dftl_tpbuf=1 &
  run leaftl12 $W mode=leaftl op=$OP cache_bytes=$B &
  run mu12 $W mode=mu op=$OP cache_bytes=$B &
  run fast trace=$D/$t.csv capacity=$((LF * 4096)) P=$P mode=fast op=$OPF &
done
wait
python3 - <<'PY'
import json
pols = [('V3_18.75%', 'P12'), ('page4k', 'page4k'), ('DFTL', 'dftl12'), ('LeaFTL', 'leaftl12'), ('FAST', 'fast'), ('uFTL', 'mu12'), ('IU+RMW', 'iu_rmw')]
out = ['ftlsim WAF on the sampled traces, NVMeVirt device geometry (NB=373, device L and P)',
       f"{'trace':8s} " + ' '.join(f'{n:>10s}' for n, _ in pols) + '   NB']
for t in ['spfresh', 'kv_w1', 'ali1', 'ali3', 'ali2', 'ssd']:
    row, nb = [], set()
    for _, f in pols:
        try: d = json.load(open(f'results/op7s_dev/{t}/{f}.json')); row.append(f"{d['WAF']:10.3f}"); nb.add(d['NB'])
        except Exception: row.append(f"{'-':>10s}")
    out.append(f'{t:8s} ' + ' '.join(row) + f"   {sorted(nb)}")
open('results/op7s_dev/SUMMARY.txt', 'w').write('\n'.join(out) + '\n'); print('\n'.join(out))
PY
