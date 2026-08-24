#!/usr/bin/env python3
"""Golden test: the C learner must match the Python prototype exactly.

Exports confirmed arrivals from an NVS dump, feeds both implementations the
same days in order, and diffs slots/confidence/anomaly state.

Usage: python3 tools/test_learner.py <nvs_dump> [<nvs_dump> ...]
"""
import subprocess
import sys
import tempfile
import os

sys.path.insert(0, os.path.dirname(__file__))
from learn_schedule import *
from parse_nvs import load_nvs, decode_events
import datetime

LEARNER_MAX_ARR = 96  # must match the C learner's cap

def export(days):
    lines = []
    for d in days:
        by = {}
        for t, r, mm, _ in d['events']:
            if t == 1:
                by.setdefault(r, []).append(mm)
        for r, arr in sorted(by.items()):
            arr = dedupe(arr)[:LEARNER_MAX_ARR]
            lines.append(f'{ROUTES[r]} {d["dt"]} {len(arr)} ' + ' '.join(str(m) for m in arr))
    return '\n'.join(lines) + '\n'

def reference(days):
    models = {num: RouteModel(num) for num in ROUTES}
    for d in days:
        by = {}
        for t, r, mm, _ in d['events']:
            if t == 1:
                by.setdefault(r, []).append(mm)
        for r, arr in sorted(by.items()):
            models[ROUTES[r]].learn_day(d['dt'], dedupe(arr)[:LEARNER_MAX_ARR], d['date'])
    out = []
    for num in ROUTES:
        for dt in (DAY_WEEKDAY, DAY_WEEKEND):
            slots = models[num].slots(dt)
            conf = models[num].confidence(dt)
            anom = models[num].anomaly_days
            s = ' '.join(f'{int(s[0])}:{s[1]}' for s in slots)
            out.append(f'route {num} dt {dt} slots {len(slots)} conf {conf:.4f} anom {anom}' + (f' {s}' if s else ''))
    return '\n'.join(out) + '\n'

def main():
    paths = sys.argv[1:] or ['nvs.bin']
    days = []
    for path in paths:
        ns_map, blobs = load_nvs(path)
        for (ns, key), blob in blobs.items():
            if ns != 4 or not key.startswith('d'):
                continue
            evs = decode_events(blob)
            y, m, d = int(key[1:5]), int(key[5:7]), int(key[7:9])
            date = datetime.date(y, m, d)
            days.append({'date': date, 'dt': daytype(date, set(), any(h for _,_,_,h in evs)),
                         'events': evs})
    days.sort(key=lambda x: x['date'])

    with tempfile.TemporaryDirectory() as td:
        inp = os.path.join(td, 'input.txt')
        open(inp, 'w').write(export(days))
        c_out = os.path.join(td, 'c.txt')
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        subprocess.run(['cc', '-o', os.path.join(td, 'lt'),
                        os.path.join(root, 'tools/host_test.c'),
                        os.path.join(root, 'espidf/src/learner/learner.c')], check=True)
        with open(c_out, 'w') as f:
            subprocess.run([os.path.join(td, 'lt')], stdin=open(inp), stdout=f, check=True)
        py_out = os.path.join(td, 'py.txt')
        open(py_out, 'w').write(reference(days))

        c = open(c_out).read()
        py = open(py_out).read()
        if c == py:
            print(f'GOLDEN TEST PASS: C matches Python exactly '
                  f'({len(days)} days, {len(c.splitlines())} route-daytypes)')
            return 0
        print('MISMATCH:')
        for cl, pl in zip(c.splitlines(), py.splitlines()):
            if cl != pl:
                print(f'  C: {cl[:100]}')
                print(f'  PY:{pl[:100]}')
        return 1

if __name__ == '__main__':
    sys.exit(main())