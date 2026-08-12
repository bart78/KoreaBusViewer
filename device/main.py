import time
import random

import config


def wifi_connect():
    global connected, wlan
    if not config.WIFI_SSID:
        return
    import network
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    wlan.connect(config.WIFI_SSID, config.WIFI_PASSWORD)
    deadline = time.time() + 30
    while time.time() < deadline:
        if wlan.isconnected():
            connected = True
            break
        time.sleep(0.1)
    if connected:
        print("wifi ip:", wlan.ifconfig()[0])
        try:
            import ntptime
            ntptime.settime()
            print("ntp ok")
        except Exception as e:
            print("ntp failed:", e)
    else:
        print("wifi failed")


connected = False
wlan = None
if not config.SIMULATE:
    wifi_connect()

import lvgl as lv
lv.init()

import lv_config as lv_config

d = lv.screen_active().get_display()
_draw_buf = bytearray(320 * 480 * 2)
_draw_buf2 = bytearray(320 * 480 * 2)
d.set_buffers(_draw_buf, _draw_buf2, len(_draw_buf), lv.DISPLAY_RENDER_MODE.PARTIAL)

from ui import BusUI
ui = BusUI()

from bus_api import fetch_route_arrival, discover_stop_routes, demo_data, BusApiError

KST_OFFSET = 9 * 3600


def kst_now():
    return time.time() + KST_OFFSET


def kst_struct(ts=None):
    return time.localtime(ts if ts is not None else kst_now())


connected = False
wlan = None
last_wifi_try = time.time()


def ensure_wifi():
    global connected, last_wifi_try
    if connected or not wlan or not config.WIFI_SSID:
        return
    now = time.time()
    if now - last_wifi_try < 60:
        return
    last_wifi_try = now
    try:
        if not wlan.isconnected():
            wlan.connect(config.WIFI_SSID, config.WIFI_PASSWORD)
            deadline = now + 15
            while time.time() < deadline:
                if wlan.isconnected():
                    connected = True
                    break
                time.sleep(0.1)
            if connected:
                print("wifi reconnected:", wlan.ifconfig()[0])
                import ntptime
                ntptime.settime()
    except Exception as e:
        print("wifi retry failed:", e)


routes = list(config.KNOWN_ROUTES)
last_fetch = 0
calls_today = 0
budget_day = ""


def is_night(lt):
    return lt[3] < config.START_HOUR or lt[3] >= config.END_HOUR


def is_peak(lt):
    if lt[6] not in config.PEAK_DAYS:
        return False
    for s, e in config.PEAK_WINDOWS:
        if s <= lt[3] < e:
            return True
    return False


def next_interval():
    if is_peak(kst_struct()):
        return config.PEAK_REFRESH
    lt = kst_struct()
    now_min = lt[3] * 3600 + lt[4] * 60 + lt[5]
    remaining_secs = config.END_HOUR * 3600 - now_min
    remaining_calls = config.DAILY_BUDGET - calls_today
    n = max(len(routes), 1)
    if remaining_calls <= 0:
        return 3600
    return max(120, min(3600, remaining_secs * n // remaining_calls))


def simulate_data():
    routes_out = []
    for r in config.KNOWN_ROUTES:
        a1 = random.randint(1, 25) * 60 + random.randint(0, 59)
        a2 = a1 + random.randint(5, 30) * 60
        routes_out.append({
            "route": r["name"], "kind": r["kind"],
            "arrivals": [{"secs": a1, "raw": ""}, {"secs": a2, "raw": ""}],
        })
    return {"stop_name": config.STOP_NAME, "routes": routes_out}


def refresh():
    global last_fetch, calls_today
    now = time.time()
    lt = kst_struct(now)
    if config.SIMULATE:
        ui.set_data(simulate_data(), "SIM",
                    "SIM %02d:%02d:%02d  %d routes  %dm cycle" % (
                        lt[3], lt[4], lt[5], len(routes), next_interval() // 60))
        last_fetch = now
        return
    if not config.API_KEY or not connected:
        ui.set_data(demo_data(), "DEMO", "no key / no wifi")
        last_fetch = now
        return
    collected = []
    ok = 0
    for r in routes:
        try:
            collected.append(fetch_route_arrival(config.API_KEY, config.ST_ID,
                                                 r["busRouteId"], r["ord"]))
            ok += 1
        except BusApiError:
            collected.append({"route": r["name"], "kind": r.get("kind", ""),
                              "arrivals": [], "error": True})
        except Exception:
            collected.append({"route": r["name"], "kind": r.get("kind", ""),
                              "arrivals": [], "error": True})
    calls_today += len(routes)
    note = "UPD %02d:%02d:%02d  %d routes  %dm" % (lt[3], lt[4], lt[5], len(routes),
                                                    next_interval() // 60)
    if ok < len(collected):
        note += "  %d ERR" % (len(collected) - ok)
    ui.set_data({"stop_name": config.STOP_NAME, "routes": collected}, "LIVE", note)
    last_fetch = now


def manual_refresh():
    global last_fetch
    now = time.time()
    if now - last_fetch < 30:
        return
    if is_night(kst_struct(now)):
        return
    refresh()


ui.on_refresh(manual_refresh)
refresh()

booted = time.time()

# render the board once, then hold the frame (driver freezes on further flushes)
for _ in range(10):
    lv.timer_handler()
    time.sleep(0.05)

while True:
    time.sleep(10)
