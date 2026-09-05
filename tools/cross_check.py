#!/usr/bin/env python3
"""Cross-check manual tap data against the board's logged confirms.

The manual tap app (https://crow-c66ca.web.app/buslog/) exports
route,date,iso8601_kst,minute_of_day[,note] CSVs. This matches each tap to the
board's confirmed arrivals for that route/day, within a tolerance window, and
reports per-route match/miss rates plus the offset distribution (calibration).

Usage: python3 tools/cross_check.py <nvs_dump> <tap_csv> [<tap_csv> ...]
"""
import sys
import csv
import statistics

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from parse_nvs import load_nvs, decode_events, ROUTES

TOL = 3  # minutes: a tap within this of a board confirm is a match


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    ns_map, blobs = load_nvs(sys.argv[1])

    taps = []
    for path in sys.argv[2:]:
        with open(path) as f:
            taps += list(csv.DictReader(f))
    if not taps:
        print('no taps found')
        return 1

    stats = {no: {'n': 0, 'matched': 0, 'offsets': []} for no in ROUTES}
    border = []
    for t in taps:
        try:
            route = int(t['route'])
            mm = int(t['minute_of_day'])
        except (KeyError, ValueError):
            continue
        if route not in stats:
            continue
        key = 'd' + t['date'].replace('-', '')
        stats[route]['n'] += 1
        if (4, key) not in blobs:
            print(f'{route} {t["date"]} {mm // 60:02d}:{mm % 60:02d}  (no board day)')
            continue
        ri = ROUTES.index(route)
        evs = decode_events(blobs[(4, key)])
        confs = sorted(m for tt, r2, m, _ in evs if tt == 1 and r2 == ri)
        if not confs:
            continue
        off = min(confs, key=lambda c: abs(c - mm)) - mm
        stats[route]['offsets'].append(off)
        if abs(off) <= TOL:
            stats[route]['matched'] += 1
            v = 'MATCH'
        else:
            v = 'MISS'
            if abs(off) <= TOL + 2:
                border.append((route, t['date'], mm, off))
        print(f'{route:>4} {t["date"]} {mm // 60:02d}:{mm % 60:02d} '
              f'off {off:+d}m  {v}')

    print()
    print(f'=== per-route (tolerance {TOL} min) ===')
    for no in ROUTES:
        s = stats[no]
        if s['n'] == 0:
            continue
        miss = s['n'] - s['matched']
        med = statistics.median(s['offsets']) if s['offsets'] else 0
        print(f'{no:>4}: {s["n"]} taps, {s["matched"]} matched, {miss} missed '
              f'({100 * miss / s["n"]:.0f}% miss rate), offset median {med:+.0f}m')
    if border:
        print()
        print('borderline (off by 4-5 min — late bus vs confirm moment?):')
        for route, date, mm, off in border:
            print(f'  {route} {date} {mm // 60:02d}:{mm % 60:02d} ({off:+d}m)')
    return 0


if __name__ == '__main__':
    sys.exit(main())