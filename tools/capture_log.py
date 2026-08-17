#!/usr/bin/env python3
"""Headless arrival logger: the board's data pipeline without the board.

Polls GBIS + TAGO for a stop with the firmware's adaptive cadence (15s peak /
60s off-peak / 20s on failure) and applies the same arrival-detection logic
(vehicle-id roll + 2-poll silence confirmations, <=240s predicted window with
per-vehicle dedupe). Writes the same 2-byte events the board logs, one daily
file per day: <out>/dYYYYMMDD.bin — readable by analyze_buslog.py and
learn_schedule.py.

Usage:
  python3 tools/capture_log.py --key API_KEY --station-id 206000648 \
      --node-id GGB206000648 [--city-code 31020] \
      [--routes 32,73,310,340,4103,9409,9507] [--out data]
"""
import argparse
import datetime
import json
import os
import sys
import time
import urllib.request

PEAK_REFRESH = 15
OFF_REFRESH = 60
RETRY_REFRESH = 20
LOG_PREDICT_SECS = 240
LOG_CONFIRM_SECS = 240
LOG_REARM_SECS = 300
PEAK_HOURS = ((6, 9), (15, 19))


def kst_now():
    return time.time() + 9 * 3600


def is_peak_now(t=None):
    if t is None:
        t = time.localtime(kst_now())
    if t.tm_wday >= 5:
        return False
    return any(t.tm_hour >= a and t.tm_hour < b for a, b in PEAK_HOURS)


def fmt_minute(minute):
    return f'{minute // 60:02d}:{minute % 60:02d}'


class Logger:
    def __init__(self, key, station_id, node_id, city_code, routes, outdir):
        self.key = key
        self.station_id = station_id
        self.node_id = node_id
        self.city_code = city_code
        self.routes = routes
        self.outdir = outdir
        os.makedirs(outdir, exist_ok=True)
        self.pend = {}          # route -> (eta, eta2, seats, veh)
        self.rows = {}          # route -> row state
        for no in routes:
            self.rows[no] = {
                'has_arrival': 0, 'arrtime': 0, 'veh': -1,
                'logged': 0, 'veh_logged': -1, 'logged_ts': 0,
                'confirm_pending': 0, 'silent_polls': 0,
                'pend_eta': 0, 'pend_fetch': 0, 'fetched': 0,
            }

    def http_get(self, url):
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=12) as r:
            return json.load(r)

    def fetch_gbis(self):
        url = (f'https://apis.data.go.kr/6410000/busarrivalservice/v2/'
               f'getBusArrivalListv2?serviceKey={self.key}&stationId={self.station_id}&format=json')
        try:
            data = self.http_get(url)
        except Exception as e:
            print(f'{time.strftime("%H:%M:%S", time.localtime(kst_now()))} GBIS fail: {e}')
            return 0
        items = data.get('response', {}).get('msgBody', {}).get('busArrivalList', [])
        for it in items:
            no = it.get('routeName')
            at = it.get('predictTimeSec1')
            if no is None or at is None:
                continue
            flag = it.get('flag') or 'PASS'
            if flag in ('STOP', 'WAIT'):
                continue
            self.pend_add(no, at,
                          it.get('predictTimeSec2'),
                          it.get('remainSeatCnt1'),
                          it.get('vehId1'))
        return 1

    def fetch_tago(self):
        url = (f'https://apis.data.go.kr/1613000/ArvlInfoInqireService/'
               f'getSttnAcctoArvlPrearngeInfoList?serviceKey={self.key}&cityCode={self.city_code}'
               f'&nodeId={self.node_id}&pageNo=1&numOfRows=20&_type=json')
        try:
            data = self.http_get(url)
        except Exception as e:
            print(f'{time.strftime("%H:%M:%S", time.localtime(kst_now()))} TAGO fail: {e}')
            return 0
        body = data.get('response', {}).get('body', {})
        items = (body.get('items') or {}).get('item', [])
        if isinstance(items, dict):
            items = [items]
        for it in items:
            no = it.get('routeno')
            at = it.get('arrtime')
            if no is not None and at is not None:
                self.pend_add(no, at, None, None, -1)
        return 1

    def pend_add(self, no, at, at2, seats, veh):
        cur = self.pend.get(no)
        if cur is None:
            self.pend[no] = [at, at2, seats, veh]
            return
        if at < cur[0]:
            cur[0], cur[3] = at, veh
        if at2 is not None and cur[1] is not None and at2 < cur[1]:
            cur[1] = at2
        if seats is not None and cur[2] is not None and seats > cur[2]:
            cur[2] = seats

    def log_arrival(self, route, arrival_ts, typ):
        t = time.localtime(arrival_ts)
        minute = t.tm_hour * 60 + t.tm_min
        ri = self.routes.index(route)
        ev = (typ << 14) | (ri << 11) | (minute & 0x7FF)
        key = f'd{t.tm_year:04d}{t.tm_mon:02d}{t.tm_mday:02d}'
        path = os.path.join(self.outdir, key + '.bin')
        with open(path, 'ab') as f:
            f.write(bytes((ev & 0xFF, ev >> 8)))
        print(f'{time.strftime("%H:%M:%S", time.localtime(kst_now()))} logged arrival: '
              f'route {route} at {fmt_minute(minute)} ({"confirmed" if typ else "predicted"})')

    def apply_rows(self):
        now = kst_now()
        for no in self.routes:
            r = self.rows[no]
            old_has, old_eta, old_fetch, old_veh = r['has_arrival'], r['arrtime'], r['fetched'], r['veh']
            p = self.pend.get(no)
            r['has_arrival'] = 1 if p else 0
            r['arrtime'] = p[0] if p else 0
            r['veh'] = p[3] if p else -1
            r['fetched'] = now

            if old_has and old_eta <= LOG_CONFIRM_SECS:
                roll = r['has_arrival'] and old_veh >= 0 and r['veh'] >= 0 and r['veh'] != old_veh
                if roll:
                    self.log_arrival(no, old_fetch + old_eta, 1)
                    r['confirm_pending'], r['silent_polls'] = 0, 0
                elif not r['has_arrival']:
                    if not r['confirm_pending']:
                        r['pend_eta'], r['pend_fetch'] = old_eta, old_fetch
                        r['confirm_pending'], r['silent_polls'] = 1, 0
                    r['silent_polls'] += 1
                    if r['silent_polls'] >= 2:
                        self.log_arrival(no, r['pend_fetch'] + r['pend_eta'], 1)
                        r['confirm_pending'] = 0
                else:
                    r['confirm_pending'], r['silent_polls'] = 0, 0
            elif r['confirm_pending'] and not r['has_arrival']:
                r['silent_polls'] += 1
                if r['silent_polls'] >= 2:
                    self.log_arrival(no, r['pend_fetch'] + r['pend_eta'], 1)
                    r['confirm_pending'] = 0
            else:
                r['confirm_pending'], r['silent_polls'] = 0, 0

            if r['has_arrival']:
                if r['arrtime'] <= LOG_PREDICT_SECS:
                    dup = r['logged'] and r['veh_logged'] == r['veh']
                    recent = r['logged_ts'] and now + r['arrtime'] - r['logged_ts'] < 120
                    if not dup and not recent:
                        self.log_arrival(no, now + r['arrtime'], 0)
                        r['logged'], r['veh_logged'], r['logged_ts'] = 1, r['veh'], now + r['arrtime']
                elif r['arrtime'] > LOG_REARM_SECS:
                    r['logged'] = 0
            else:
                r['logged'] = 0

    def run(self):
        fetch_fail = 0
        last_refresh = 0
        while True:
            now = kst_now()
            if now - last_refresh >= (RETRY_REFRESH if fetch_fail else
                                      PEAK_REFRESH if is_peak_now() else OFF_REFRESH):
                self.pend.clear()
                ok = self.fetch_gbis()
                ok |= self.fetch_tago()
                if not ok:
                    fetch_fail += 1
                else:
                    fetch_fail = 0
                self.apply_rows()
                last_refresh = now
            time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--key', required=True)
    ap.add_argument('--station-id', default='206000648')
    ap.add_argument('--node-id', default='GGB206000648')
    ap.add_argument('--city-code', default='31020')
    ap.add_argument('--routes', default='32,73,310,340,4103,9409,9507')
    ap.add_argument('--out', default='data')
    a = ap.parse_args()
    routes = [int(x) for x in a.routes.split(',')]
    lg = Logger(a.key, a.station_id, a.node_id, a.city_code, routes, a.out)
    print(f'capture started: {len(routes)} routes -> {a.out}/  (peak 15s / off 60s)')
    try:
        lg.run()
    except KeyboardInterrupt:
        print('stopped')


if __name__ == '__main__':
    main()
