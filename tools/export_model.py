#!/usr/bin/env python3
"""Export the learned ring model to JSON for offline consumers (e-paper
dashboard, etc.). No API keys, no network — just the model.

Each slot carries:
  med   - the expected arrival (minute of day) — the ring's median
  early - the earliest observed arrival near this slot: the conservative
          'leave by' bound for catching the bus (apply your own margin)
  late  - the latest observed arrival near this slot: together with early
          this is the deterministic window — 'arrives between early and
          late' holds for every observed day (add a margin for unseen days)
  n     - days with an arrival near this slot
  q     - slot quality (fraction of those days within +/-3 min)

Consumers should claim only slots with q >= 0.60 (the board's own gate) and
show '--' otherwise. Day-type: weekday vs weekend+holiday (holidays observed
by the board are listed; merge in your own calendar for upcoming ones).

Usage: python3 tools/export_model.py <nvs_dump> <out.json>
"""
import sys
import json
import datetime

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from parse_nvs import load_nvs, decode_events, ROUTES
from learn_schedule import (RouteModel, dedupe, DAY_WEEKDAY, DAY_WEEKEND,
                            MERGE_GAP)

DT_NAME = {DAY_WEEKDAY: 'weekday', DAY_WEEKEND: 'weekend'}


def slot_samples(model, daytype, med):
    out = []
    for d in model.ring[daytype]:
        near = [m for m in d if abs(m - med) <= MERGE_GAP]
        if near:
            out.append(min(near, key=lambda m: abs(m - med)))
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ns_map, blobs = load_nvs(sys.argv[1])
    today = datetime.date.today()   # exclude today: the model runs through yesterday

    holidays = set()
    days = []
    for (ns, key), blob in blobs.items():
        if ns != 4 or not key.startswith('d'):
            continue
        evs = decode_events(blob)
        y, m, d = int(key[1:5]), int(key[5:7]), int(key[7:9])
        date = datetime.date(y, m, d)
        hol = any(h for _, _, _, h in evs)
        if hol:
            holidays.add(date)
        days.append((date, hol, evs))
    days.sort()

    out = {
        'stop': 'GGB206000648',
        'exported': (datetime.datetime.now(datetime.timezone.utc)
                     + datetime.timedelta(hours=9)).strftime('%Y-%m-%dT%H:%M+09:00'),
        'model_through': str(max(d for d, _, _ in days)),
        'holidays_observed': sorted(d.strftime('%Y-%m-%d') for d in holidays),
        'routes': {},
    }

    for no in ROUTES:
        model = RouteModel(no)
        for date, hol, evs in days:
            if date >= today:
                continue
            dt = DAY_WEEKEND if hol or date.weekday() >= 5 else DAY_WEEKDAY
            ri = ROUTES.index(no)
            arr = dedupe([mm for t, r2, mm, _ in evs if t == 1 and r2 == ri])
            if arr:
                model.learn_day(dt, arr, date)
        rt = {}
        for dt in (DAY_WEEKDAY, DAY_WEEKEND):
            slots = model.slots(dt)
            sl = []
            for med, n in slots:
                samples = slot_samples(model, dt, med)
                early = min(samples) if samples else med
                late = max(samples) if samples else med
                q = model.slot_quality(dt, med)
                sl.append({'med': med, 'early': early, 'late': late,
                           'n': n, 'q': round(q, 3)})
            rt[DT_NAME[dt]] = {
                'conf': round(model.confidence(dt), 3),
                'slots': sl,
            }
        out['routes'][str(no)] = rt

    with open(sys.argv[2], 'w') as f:
        json.dump(out, f, indent=1)
    n = sum(len(rt[d]['slots']) for rt in out['routes'].values() for d in rt)
    print(f'exported {len(out["routes"])} routes, {n} slots -> {sys.argv[2]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())