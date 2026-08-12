# KoreaBusViewer

An ESP32-S3 signboard that shows **live bus arrivals** for a single Korean (Seongnam/Gyeonggi) bus stop on a 320×480 QSPI display — arrival countdowns with seconds, second-bus and seat-count subtext, a schedule fallback when live data is silent, and a self-validating arrival logger that records ground truth for future prediction.

The board this was built for: JC3248W535EN (ESP32-S3, 320×480 LVGL display).

## 1. Description

The signboard polls two Korean transit APIs and merges them into one arrival list for a configured stop:

- **GBIS** (경기버스정보시스템, data.go.kr service `6410000`) — primary. Returns per-route first/second bus ETA **in seconds**, remaining seats, congestion, current bus location and vehicle IDs.
- **TAGO** (국토교통부 버스도착정보, service `1613000`) — fallback for routes GBIS reports empty. Returns per-vehicle arrivals.

Neither API covers every route, so they complement each other (details in "Things tried"). The earliest ETA per route wins.

Display semantics:

| What you see | Meaning |
|---|---|
| White `4m 10s` | Fresh live arrival (with seconds) |
| Yellow / orange | Live data is 1–3 min / 3+ min old |
| Subtext `+9m` | Second bus ETA (regular routes) |
| Subtext `SEATS 12` | Seats left (express routes only) |
| Blue `~2Xm` | Schedule estimate; counts down, **never** says SOON |
| `--` | Schedule estimate inside the live-reporting window with no live sighting — if a bus were really that close, live data would confirm it |
| `DONE` | Last bus has passed the stop |
| Header orange | Both feeds failing (keeps last data, retries, auto-reconnects WiFi) |
| Header `NOLOG`/`LOG!` (yellow) | Arrival logger problem |

## 2. Implementation

- **Firmware**: ESP-IDF 5.x + LVGL 8.3, built with PlatformIO (`espidf/`). Single main module `src/bus.c`.
- **Data flow**: a refresh task polls GBIS then TAGO on an adaptive cadence (15 min peak → 15 s, off-peak → 60 s, failures → 20 s). Responses are parsed item-by-item (order-independent, quoted/unquoted numbers, `null`-safe) and merged into a per-route "earliest ETA" table that drives both the display and the logger.
- **Schedule fallback**: static per-route rows (headway by day type, first/last bus, ride time) synthesize a next-departure countdown. The countdown is only shown when it is **outside** the live-reporting window; inside the window, live silence means "no bus coming" and the row shows `--`.
- **Arrival logger**: every arrival is appended to NVS as a 2-byte event, one blob per day (60 days retained):
  - `type 0` = **predicted** arrival (first-bus ETA crossed ≤ 90 s)
  - `type 1` = **confirmed** arrival (the bus vanished from the feed — the closest thing to ground truth without standing at the stop)
  - Event = `type(1) | route(3) | minute-of-day(11)`, deduped per vehicle ID.
  - Matching predicted↔confirmed pairs per route yields the prediction-error distribution and phantom rate — the basis for a future learned arrival fallback.
- **Robustness**: WiFi auto-reconnect (with SNTP resync) every 30 s, per-feed failure isolation, response truncation detection, `NOLOG`/`LOG!`/orange status in the header.

## 3. Things tried

This repo is the result of a long debugging journey; the interesting bits:

- **The encoding rabbit hole**: the original timetable CSV was garbled (Japanese-looking mojibake). It turned out to be Korean EUC-KR bytes re-read as Shift-JIS, then saved as UTF-8 — decoded via `garbled.encode('euc_jp').decode('euc_kr')` → clean hangul (기준일자, 노선명, …). The file turned out to be **OD passenger data** (boarding/alighting stop pairs + passenger counts), not a timetable.
- **The off-by-one arrival bug**: every route displayed the *next* route's ETA — the JSON items list `arrtime` before `routeno`, and the first parser searched forward from the `routeno` match. Rewrote the parser to extract both keys within the same item span.
- **Quoted vs unquoted numbers**: data.go.kr returns `"routeno":9507` (unquoted) in one service and `"routeName":9507` with `null`s in another — the parser had to handle both, plus `null`/empty values.
- **Coverage gaps**: route 32 had live data on Kakao (GBIS) but nothing on TAGO; route 4103 was the mirror image (TAGO live, Kakao silent). No single feed covers everything → merge both, earliest wins.
- **"It's 6 minutes away"**: the board once showed a live 6-minute ETA for a route whose last bus had already passed — same off-by-one bug, now covered by the "inside the live window without confirmation → `--`" rule.
- **Schedule estimates that lied**: the headway model happily claimed `~2m` at 11 pm when no bus existed. Rule: only show estimates outside the live-reporting window; never say SOON in blue.
- **A learning system was considered** (predict arrivals from observed history) and deferred — instead the firmware now logs raw predicted/confirmed arrivals per bus, so the decision can be made later on real data instead of assumptions.

## 4. Getting it up and running

Prerequisites: PlatformIO (ESP-IDF 5.x toolchain), a free data.go.kr account.

1. Register the APIs on data.go.kr (both auto-approve) and note the service key:
   - 국토교통부_(TAGO)_버스도착정보 (`1613000`) — optional fallback
   - 경기도_버스도착정보 조회 (`6410000`, endpoint `busarrivalservice/v2`) — primary
2. Configure the board:
   ```bash
   cd espidf
   cp src/secret_config.h.example src/secret_config.h   # WiFi + API key
   ```
3. Point it at your stop: `NODE_ID`/`GBIS_STATION_ID`/`STOP_LABEL` in `src/bus.c`, and the route table `ROWS[]` (headways, first/last bus, ride time). To find your GBIS station ID: `https://m.gbis.go.kr/api/stationSearch?keyword=<stop name>`.
4. Build and flash:
   ```bash
   pio run -t upload
   ```
5. Watch logs: `pio device monitor` (arrivals log as `logged arrival: route 32 at 18:45 (confirmed/predicted)`).

The MicroPython prototype lives in `device/` (copy `config.py.example` → `config.py`).

## 5. Credits

- **Board**: JC3248W535EN 320×480 QSPI display module ([vendor listing](https://s.click.aliexpress.com/e/_DFO5uIV)); vendor demo code kept under `espidf/JC3248W535EN/` (not committed)
- **[LVGL](https://github.com/lvgl/lvgl)** — vendored under `espidf/libraries/lvgl`
- **[straga/micropython_lcd](https://github.com/straga/micropython_lcd)** (Kevin G. Schlosser) — MicroPython prototype display drivers
- **[lvgl/lv_micropython](https://github.com/lvgl/lv_micropython)** — prototype firmware image
- **Data**: 국토교통부 버스도착정보 (TAGO) and 경기도 버스정보시스템 (GBIS) via data.go.kr
