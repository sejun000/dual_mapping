#!/usr/bin/env python3
"""WAF of a DOGI run in the ftlsim convention: (user + GC programs in the measurement window) / host pages submitted
in the window (page4k export json `host_pages`, i.e. before the 1 MiB write buffer).
  tools/dogi_v3_waf.py <results/dogi_v3/<wl>> [mode ...]  -> writes dogi_<mode>.json, prints one line per mode."""
import json, re, sys, os

d = sys.argv[1]
modes = sys.argv[2:] or ['strict']
ex = json.load(open(os.environ.get('EXPORT') or os.path.join(d, 'page4k_export.json')))
H, Hftl = ex['host_pages'], ex['host_pages_ftl']
for m in modes:
    log = os.path.join(d, f'dogi_{m}.log')
    raw, port, trains, gconf = {}, {}, [], []
    mark_t = None
    last_uw, last_tw, after_mark = None, None, False
    for ln in open(log, errors='replace'):
        r = re.match(r'UserWrite: ([0-9.]+) GiB', ln)
        if r: last_uw = float(r.group(1))
        r = re.match(r'Per20GiB WAF: [0-9.]+, Total WAF: ([0-9.]+)', ln)
        if r and after_mark: last_tw = (last_uw, float(r.group(1)))
        if ln.startswith('[RAW MARK]'): after_mark = True
        r = re.match(r'\[RAW (\w+)\] user=(\d+) gc=(\d+)(?: used_segs=(-?\d+) peak_segs=(\d+) over_cap=(\d+) stall_us=(\d+)(?: emergency=(\d+))?)?', ln)
        if r:
            raw[r.group(1)] = dict(user=int(r.group(2)), gc=int(r.group(3)),
                                   **({} if r.group(4) is None else dict(used=int(r.group(4)), peak=int(r.group(5)),
                                                                        over=int(r.group(6)), stall_us=int(r.group(7)),
                                                                        emergency=int(r.group(8) or 0))))
        r = re.match(r'\[PORT\] MARK at logicalTime=(\d+) trainings=(\d+) APPLY_ML=(\d+)', ln)
        if r: mark_t, port['mark_trainings'], port['mark_apply_ml'] = int(r.group(1)), int(r.group(2)), int(r.group(3))
        r = re.match(r'\[PORT\] training #(\d+) starts at logicalTime~(\d+)', ln)
        if r: trains.append(int(r.group(2)))
        r = re.match(r'\[GCONF\] applied from group optimizer: (.*)', ln)
        if r: gconf.append(r.group(1).strip())
        r = re.match(r'\[PORT\] END logicalTime=(\d+) trainings_done_before_end=(\d+) gconf_ok=(\d+) APPLY_ML=(\d+)', ln)
        if r: port.update(end_t=int(r.group(1)), end_trainings=int(r.group(2)), end_gconf_ok=int(r.group(3)), end_apply_ml=int(r.group(4)))
        r = re.match(r'\[PORT\] FINAL trainings=(\d+) gconf_ok=(\d+) APPLY_ML=(\d+)', ln)
        if r: port.update(final_trainings=int(r.group(1)))
    out = dict(mode=m, host_pages=H, host_pages_ftl=Hftl, page4k_WAF=ex['WAF'], P=ex['P'], NB=ex['NB'])
    if 'MARK' in raw and 'END' in raw:
        a, b = raw['MARK'], raw['END']
        du, dg = b['user'] - a['user'], b['gc'] - a['gc']
        out.update(user_window=du, gc_window=dg, WAF=(du + dg) / H, WAF_ftl=(du + dg) / du if du else None,
                   user_window_matches_export=(du == Hftl), raw=raw, mark_logical_time=mark_t,
                   trainings_started_at=trains, n_trainings_started=len(trains),
                   trainings_started_in_window=sum(1 for t in trains if mark_t is not None and t >= mark_t),
                   gconf_applied=gconf, **port)
        if 'PRE_DRAIN' in raw:
            out['WAF_pre_drain'] = (raw['PRE_DRAIN']['user'] + raw['PRE_DRAIN']['gc'] - a['user'] - a['gc']) / H
        ml = 'ML' if port.get('mark_apply_ml') else ('ML(late)' if port.get('end_apply_ml') else 'no-ML')
        out['ml_status'] = ml
        print(f"{os.path.basename(d):16s} {m:6s} WAF={out['WAF']:.4f} page4k={ex['WAF']:.4f} user_ok={out['user_window_matches_export']} "
              f"{ml} trainings={len(trains)} (in window {out['trainings_started_in_window']}) APPLY_ML@mark={port.get('mark_apply_ml')} "
              f"peak_segs={b.get('peak')} NB={ex['NB']} over_cap={b.get('over')}")
    else:
        out['error'] = 'missing MARK/END (run stopped)'
        if 'MARK' in raw and last_tw:
            # partial window from DOGI's cumulative 20 GiB report: (user+gc since MARK) / user since MARK, scaled to
            # host pages (post-buffer user pages / submitted pages of the export run)
            a = raw['MARK']
            u1 = last_tw[0] * 262144; t1 = u1 * last_tw[1]
            du = u1 - a['user']
            if du > 0:
                out['WAF_partial'] = (t1 - a['user'] - a['gc']) / du * (Hftl / H)
                out['partial_window_frac'] = du / Hftl
        print(f"{os.path.basename(d):16s} {m:6s} INCOMPLETE partial={out.get('WAF_partial')} frac={out.get('partial_window_frac')}")
    json.dump(out, open(os.path.join(d, f'dogi_{m}.json'), 'w'), indent=1)
