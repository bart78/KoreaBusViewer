#!/usr/bin/env python3
"""Offline prototype of the learned-arrival fallback for the bus board.

Validates the on-device algorithm before porting to bus.c:
  - ring model: per route / day-type, the last RING_SIZE same-type days;
    expected arrivals = aligned medians across the ring (the gradient)
  - confidence gating (live data always trumps; learned values only fill)
  - same-direction anomaly detection; the ring sliding back in replaces an
    old pattern gradually, so a schedule change flags, then self-heals,
    and a reversion back is flagged again
  - holidays: dates can be supplied and are scored as weekend day-type

Usage: python3 tools/learn_schedule.py <legacy.nvs> [<new.nvs> ...] [--holidays YYYY-MM-DD,...]
"""
import sys
import datetime
import statistics
from collections import defaultdict

from parse_nvs import load_nvs, decode_events, ROUTES

DAY_WEEKDAY, DAY_WEEKEND = 0, 1
RING_SIZE = 10
MAX_GAP = 12
MIN_SLOT_N = 3
ANOM_DEV = 8
ANOM_FRAC = 0.7
REBASELINE_DAYS = 3

HEADWAY = {32: 13, 73: 20, 310: 25, 340: 27, 4103: 40, 9409: 25, 9507: 33}

def dedupe(arrivals, gap=3):
    out = []
    last = -999
    for m in sorted(arrivals):
        if m - last >= gap:
            out.append(m)
            last = m
    return out

def daytype(date, holidays, hol_flag=False):
    if hol_flag or date in holidays:
        return DAY_WEEKEND
    return DAY_WEEKEND if date.weekday() >= 5 else DAY_WEEKDAY

MERGE_GAP = 6      # merge aligned columns closer than this (missed-bus shift)
MIN_DAY_FRAC = 0.6 # anomaly scoring needs a day at least this complete
TOLERANCE = 3      # confidence match tolerance (minutes around a slot)

def aligned_medians(days):
    if not days:
        return []
    n = max(len(d) for d in days)
    positions = []
    for i in range(n):
        col = [d[i] for d in days if i < len(d)]
        if col:
            positions.append([statistics.median(col), len(col)])
    # ordinal alignment splits one real arrival into two columns when a day
    # missed a bus (everything shifts by one position) — merge close columns
    merged = []
    for med, cnt in sorted(positions):
        if merged and med - merged[-1][0] <= MERGE_GAP:
            prev = merged[-1]
            tot = prev[1] + cnt
            prev[0] = (prev[0] * prev[1] + med * cnt) / tot
            prev[1] = tot
        else:
            merged.append([med, cnt])
    return [(round(m), int(c)) for m, c in merged]

class RouteModel:
    def __init__(self, number):
        self.number = number
        self.gap = max(6, min(MAX_GAP, HEADWAY[number] // 2))
        self.ring = {DAY_WEEKDAY: [], DAY_WEEKEND: []}
        self.anomaly_days = 0

    def learn_day(self, daytype, arrivals, date):
        arrivals = dedupe(arrivals)
        if not arrivals:
            return
        shifted, gated = self.score_day(self.ring[daytype], arrivals)
        if shifted:
            self.anomaly_days += 1
        else:
            self.anomaly_days = 0
        self.ring[daytype].append(arrivals)
        if len(self.ring[daytype]) > RING_SIZE:
            self.ring[daytype].pop(0)
        return gated

    def score_day(self, ring, arrivals):
        if len(ring) < 3:
            return False, True
        if len(arrivals) < MIN_DAY_FRAC * statistics.median(len(d) for d in ring):
            return False, True   # partial day: can't judge it
        shifts = []
        for d in ring:
            m = min(len(d), len(arrivals))
            if m < 5:
                continue
            shifts.append(statistics.median(a - b for a, b in zip(arrivals[:m], d[:m])))
        if len(shifts) < 3:
            return False, True
        med = statistics.median(shifts)
        same = sum(1 for s in shifts if (s > 0) == (med > 0))
        shifted = abs(med) > ANOM_DEV and same >= 0.6 * len(shifts)
        return shifted, False

    def slots(self, daytype):
        return aligned_medians(self.ring[daytype])

    def predict_next(self, daytype, t):
        slots = self.slots(daytype)
        nxt = [s[0] for s in slots if s[0] > t]
        if not nxt:
            return None, None, None
        a = nxt[0]
        n2 = [s[0] for s in slots if s[0] > a + 1]
        b = n2[0] if n2 else None
        n_a = next(s[1] for s in slots if abs(s[0] - a) < 0.01)
        n_b = next((s[1] for s in slots if b is not None and abs(s[0] - b) < 0.01), 0)
        return a, b, min(n_a, n_b) if b is not None else n_a

    def confidence(self, daytype):
            """Reproducibility: for each merged slot, how tight are the days'
            arrivals that land near it? Days with no arrival near a slot (a
            missed bus) are skipped, not punished. Learned fills are only used
            above a threshold."""
            slots = self.slots(daytype)
            ring = self.ring[daytype]
            if len(ring) < 3 or not slots:
                return 0
            matched = 0
            total = 0
            for med, _ in slots:
                for d in ring:
                    near = [m for m in d if abs(m - med) <= MERGE_GAP]
                    if not near:
                        continue
                    total += 1
                    if min(abs(m - med) for m in near) <= TOLERANCE:
                        matched += 1
            return matched / total

    def slot_quality(self, daytype, med):
        """Same tightness measure, for one slot median (the display gates
        claims per slot: a loose slot withholds itself)."""
        ring = self.ring[daytype]
        if len(ring) < 3:
            return 0
        matched = 0
        total = 0
        for d in ring:
            near = [m for m in d if abs(m - med) <= MERGE_GAP]
            if not near:
                continue
            total += 1
            if min(abs(m - med) for m in near) <= TOLERANCE:
                matched += 1
        return matched / total if total else 0

def fmt(m):
    return f'{int(m // 60):02d}:{int(m % 60):02d}'

def load_days(paths):
    days = []
    for path in paths:
        ns_map, blobs = load_nvs(path)
        for (ns, key), blob in blobs.items():
            if ns != 4 or not key.startswith('d'):
                continue
            y, m, d = int(key[1:5]), int(key[5:7]), int(key[7:9])
            evs = decode_events(blob)
            days.append({'date': datetime.date(y, m, d),
                         'hol': any(h for _, _, _, h in evs),
                         'events': evs})
    return sorted(days, key=lambda x: x['date'])

def main():
    args = sys.argv[1:]
    holidays = set()
    if '--holidays' in args:
        i = args.index('--holidays')
        holidays = {datetime.date.fromisoformat(x) for x in args[i + 1].split(',')}
        args = args[:i] + args[i + 2:]
    days = load_days(args or ['nvs.bin'])
    models = {num: RouteModel(num) for num in ROUTES}

    print(f'Loaded {len(days)} days: ' + ', '.join(f"{d['date'].strftime('%a %m/%d')} ({len(d['events'])})" for d in days))
    print()

    print('=== Learning pass ===')
    for d in days:
        by_route = defaultdict(list)
        for t, r, m, h in d['events']:
            if t == 1:
                by_route[r].append(m)
        dt = daytype(d['date'], holidays, d['hol'])
        for r, arrivals in by_route.items():
            num = ROUTES[r]
            gated = models[num].learn_day(dt, arrivals, d['date'])
            if d['date'].weekday() < 5:
                print(f"  {d['date'].strftime('%a')} route {num}: {len(dedupe(arrivals))} arrivals, "
                      f"{models[num].anomaly_days} anomaly day{'s' if models[num].anomaly_days != 1 else ''}"
                      f"{' [SCHEDULE CHANGE FLAG]' if models[num].anomaly_days >= REBASELINE_DAYS else ''}"
                      f"{' [no baseline yet]' if gated else ''}")
    print()

    print('=== Learned weekday schedule (per-route slots, ring medians) ===')
    for num in ROUTES:
        m = models[num]
        slots = m.slots(DAY_WEEKDAY)
        if not slots:
            print(f'Route {num}: no weekday data')
            continue
        desc = ', '.join(f'{fmt(s[0])}(n={s[1]})' for s in slots)
        learned = sum(1 for s in slots if s[1] >= MIN_SLOT_N)
        print(f'Route {num}: {len(slots)} slots ({learned} with n>={MIN_SLOT_N}), confidence {m.confidence(DAY_WEEKDAY):.0%}')
        print(f'    {desc}')
    print()

    print('=== Fill demo: learned next bus + next-next at 07:00 weekday (rule 3/4) ===')
    for num in ROUTES:
        m = models[num]
        a, b, n = m.predict_next(DAY_WEEKDAY, 7 * 60)
        if a is None:
            continue
        conf = 'learned' if n >= MIN_SLOT_N else f'low(n={n})'
        print(f'Route {num}: next ~{fmt(a)}, next+ ~{fmt(b) if b else "?"} [{conf}]')

if __name__ == '__main__':
    main()
