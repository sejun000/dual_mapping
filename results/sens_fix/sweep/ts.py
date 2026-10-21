import csv,sys
d0=sys.argv[1]; step=int(sys.argv[3]) if len(sys.argv)>3 else 10
for b in sys.argv[2].split(','):
    rows=list(csv.DictReader(open(f'{d0}/{b}.ts')))
    print(b, len(rows))
    prev=None
    for i,r in enumerate(rows):
        if i%step: continue
        r={k:float(v) for k,v in r.items()}
        if prev:
            dh=r['host_pages']-prev['host_pages']
            d=lambda k:(r[k]-prev[k])/dh
            print(f"{int(r['host_pages']):>10} waf={(d('fold_prog')+d('clean_prog')+d('refold_prog')+d('finegc_prog')+d('host_prog')):.2f} uni={int(r['uni'])} fb={int(r['fine_blocks'])} free={int(r['free'])} fv={int(r['fv'])} ct={int(r['ct_used'])} ev={d('evict')*1000:.2f} gf={d('gfold')*1000:.2f} forced={int(r['forced'])-int(prev['forced'])} clean={d('clean_prog'):.2f} refold={d('refold_prog'):.2f} fgc={d('finegc_prog'):.2f} fold={d('fold_prog'):.2f}")
        prev=r
