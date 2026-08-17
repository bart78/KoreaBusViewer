#!/usr/bin/env python3
"""Analyze buslog NVS dump: weekday arrival-time conclusions + prediction accuracy."""
import sys
import datetime
import statistics
from collections import defaultdict

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from parse_nvs import load_nvs, decode_events, ROUTES

ROUTE_NAMES = {32: '32', 73: '73', 310: '310', 340: '340', 4103: '4103', 9409: '9409', 9507: '9507'}

def fmt(minute):
    return f'{minute // 60:02d}:{minute % 60:02d}'

def load(path):
    ns_map, blobs = load_nvs(path)
    days = []
    for (ns, key), blob in blobs.items():
        if ns != 4 or not key.startswith('d'):
            continue
        y, m, d = int(key[1:5]), int(key[5:7]), int(key[7:9])
        wd = datetime.date(y, m, d).weekday()  # 0=Mon..6=Sun
        evs = decode_events(blob)
        hol = any(h for _, _, _, h in evs)   # public holiday flag (bit 15)
        days.append({'key': key, 'date': datetime.date(y, m, d), 'wd': wd,
                     'hol': hol, 'events': evs})
    return sorted(days, key=lambda x: x['key'])

def match_bus(route_events):
    """Match predicted (t=0) to confirmed (t=1) arrivals of the same bus per route.
    Predicted logged at moment ETA crossed <=90s; confirmed logged when the bus
    vanished (within 4 min of arrival). Match each predicted to the first
    confirmed at/after it within 10 min. Return (errors_min, phantoms, misses)."""
    preds = [m for t, m in route_events if t == 0]
    confs = sorted(m for t, m in route_events if t == 1)
    errors, used = [], set()
    phantoms = []
    for p in sorted(preds):
        for i, c in enumerate(confs):
            if i in used or c < p:
                continue
            if c - p <= 10:
                errors.append(c - p)
                used.add(i)
                break
        else:
            if all(c < p for c in confs):
                phantoms.append(p)
    misses = [c for i, c in enumerate(confs) if i not in used]
    return errors, phantoms, misses

def main(path):
    days = load(path)
    weekdays = [d for d in days if d['wd'] < 5 and not d['hol']]
    print(f'Days in NVS: {len(days)} (weekdays: {len(weekdays)})')
    for d in days:
        print(f"  {d['key']} {d['date'].strftime('%a')}: {len(d['events'])} events")
    print()

    per_route_days = defaultdict(list)   # route -> list of confirmed minutes per day
    per_route_errors = defaultdict(list)
    per_route_phantoms = defaultdict(list)
    per_route_misses = defaultdict(list)
    pred_by_route = defaultdict(int)
    conf_by_route = defaultdict(int)

    for d in weekdays:
        by_route = defaultdict(list)
        for t, r, m, h in d['events']:
            by_route[r].append((t, m))
        for r, evs in by_route.items():
            num = ROUTES[r]
            errors, phantoms, misses = match_bus(evs)
            confs = sorted(m for t, m in evs if t == 1)
            preds = [m for t, m in evs if t == 0]
            per_route_days[num].append(confs)
            per_route_errors[num].extend(errors)
            per_route_phantoms[num].extend(phantoms)
            per_route_misses[num].extend(misses)
            pred_by_route[num] += len(preds)
            conf_by_route[num] += len(confs)

    print('=== Weekday confirmed arrivals (the actual bus moments we observed) ===')
    for num in ROUTES:
        if num not in per_route_days:
            continue
        day_lists = per_route_days[num]
        n = sum(len(x) for x in day_lists)
        if n == 0:
            continue
        all_min = sorted(m for x in day_lists for m in x)
        med = statistics.median(all_min)
        lo, hi = min(all_min), max(all_min)
        span = (hi - lo) // 60 + 1
        print(f'Route {ROUTE_NAMES[num]}: {n} arrivals over {len(day_lists)} weekdays '
              f'({min(len(x) for x in day_lists)}-{max(len(x) for x in day_lists)}/day), '
              f'span {fmt(lo)}-{fmt(hi)} (~{span}h), median {fmt(int(med))}')
        # per-day listing
        for d, mlist in zip(weekdays, day_lists):
            if not mlist:
                print(f"   {d['date'].strftime('%a %m/%d')}: none")
            else:
                print(f"   {d['date'].strftime('%a %m/%d')}: " + ', '.join(fmt(m) for m in mlist))
        print()

    print('=== Prediction accuracy (predicted arrival vs confirmed, same-bus match) ===')
    for num in ROUTES:
        errs = per_route_errors[num]
        if not errs:
            continue
        bias = statistics.mean(errs)
        print(f'Route {ROUTE_NAMES[num]}: n={len(errs)} pairs, mean err {bias:+.1f} min, '
              f'median {statistics.median(errs):+.0f} min, p50/p95 abs err '
              f'{sorted(abs(e) for e in errs)[len(errs)//2]}/'
              f'{sorted(abs(e) for e in errs)[int(len(errs)*0.95)-1]} min')
    print()

    print('=== Phantom / missed arrivals (per route, 3 weekdays combined) ===')
    for num in ROUTES:
        ph = per_route_phantoms[num]
        ms = per_route_misses[num]
        pr = pred_by_route[num]
        if pr or ms:
            print(f'Route {ROUTE_NAMES[num]}: predicted={pr}, confirmed={conf_by_route[num]}, '
                  f'unmatched predicted (phantom)={len(ph)}, unpaired confirmed={len(ms)}'
                  + (f' phantom@' + ','.join(fmt(m) for m in ph[:8]) if ph else ''))

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'nvs.bin')
