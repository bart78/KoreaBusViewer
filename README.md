# KoreaBusViewer

An ESP32-S3 signboard that shows **live bus arrivals** for a single Korean (Seongnam/Gyeonggi) bus stop on a 320×480 QSPI display — arrival countdowns with seconds, second-bus and seat-count subtext, an **on-device learned-schedule fallback** that rebuilds arrival patterns from observed ground truth when live data goes silent, and a self-validating arrival logger.

The board this was built for: JC3248W535EN (ESP32-S3, 320×480 LVGL display).

## 1. Description

The signboard polls two Korean transit APIs and merges them into one arrival list for a configured stop:

- **GBIS** (경기버스정보시스템, data.go.kr service `6410000`) — primary. Returns per-route first/second bus ETA **in seconds**, remaining seats, congestion, current bus location and vehicle IDs.
- **TAGO** (국토교통부 버스도착정보, service `1613000`) — fallback for routes GBIS reports empty. Returns per-vehicle arrivals.

Neither API covers every route, so they complement each other (details in "Things tried"). The earliest ETA per route wins.

Display semantics:

| What you see | Meaning |
|---|---|
| White `4m 10s` | Fresh live arrival (with seconds; counts to `0m 00s`) |
| `ARRIVING` | Live bus at the stop, countdown exhausted — lasts only until the next poll confirms the pass (polling speeds to 15 s near arrivals) |
| Yellow / orange | Live data is 1–3 min / 3+ min old |
| Subtext `+9m` | Second bus ETA — live when the feed is fresh, otherwise the **learned** next bus (blue) |
| Subtext `SEATS 12` | Seats left (express routes only) |
| Blue `~2Xm` | **Learned-schedule** estimate (from on-device arrival rings); counts down, **never** says SOON |
| `--` | No bus claimed: live silence inside the 5-minute window, or a learned estimate below the confidence gate |
| `DONE` | Last bus has passed the stop |
| Header orange | Both feeds failing (keeps last data, retries, auto-reconnects WiFi) |
| Header `NOLOG`/`LOG!` (yellow) | Arrival logger problem |
| Header alternates | Every 5 s the status line swaps to `PM10 12|PM2.5 7|22C CLD` (air quality + next weather slot; line hides until the env services are registered/approved) |
| Seats bracket | Express routes show remaining seats next to the route number, e.g. `4103 (35)` — the subtext line stays free for the second bus |

## 2. Implementation

- **Firmware**: ESP-IDF 5.x + LVGL 8.3, built with PlatformIO (`espidf/`). Single main module `src/bus.c`.
- **Data flow**: a refresh task polls GBIS then TAGO on an adaptive cadence — 45 s during the 07–10 rush, 120 s by day, 180 s in the evening, none 23:00–05:00, and a 30 s burst while a bus is within 30 s of arrival (GBIS+TAGO share a 1000 calls/day budget each). Responses are parsed item-by-item (order-independent, quoted/unquoted numbers, `null`-safe) and merged into a per-route "earliest ETA" table that drives both the display and the logger. A slower task (every 10 min) fetches the header env line — air quality from the nearest airkorea station (한국환경공단 대기오염정보, service `1062168`, station `ENV_STATION` with a province-median fallback) and the next weather slot (기상청 단기예보, service `1360000`, grid `WX_GRID_X/Y`).
- **Learned-schedule fallback**: a portable learner (`src/learner/learner.c`, no FreeRTOS/LVGL deps) rebuilds per-route arrival **rings** from the logger's buslog at boot, in PSRAM. Days are bucketed by type (weekday / weekend+holiday); per-route slots are medians of observed arrival minutes with anomaly rejection, and a **reproducibility confidence** (fraction of day×slot samples matching within ±3 min, days with no near-arrival skipped) scores each route. Learned fills show in blue when confidence ≥ 0.60 and the bus is **5–90 min out**; inside 5 min, live silence reads as unknown (`--`). When the live feed is silent, the subtext shows the learned next bus (blue) in place of the live second bus. The static headway model remains the cold-start fallback for routes below the confidence gate.
- **Arrival logger**: every arrival is appended to NVS as a 2-byte event, one blob per day (60 days retained, 128 KB NVS partition):
  - `type 0` = **predicted** arrival (first bus sighted within 4 min, deduped per vehicle id)
  - `type 1` = **confirmed** arrival — the tracked close bus left the feed. Detected by a GBIS **vehicle-id roll** (the tracked bus passed even though the next bus immediately took the first-bus slot) or by **2 consecutive silent polls** (a 1-poll feed blip is not counted as an arrival).
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
- **The logger only heard half the routes**: after a few days of logging, routes 340/9507/4103/310 had plenty of confirmed arrivals but 32/73/9409 had almost none. The confirmed event fired on *feed silence*, not arrivals — and only TAGO (which covers just 310/4103/9507 and drops vehicles at the stop) ever goes silent. GBIS (the only feed for 32/73/9409) rolls its first-bus slot straight to the next bus, so those routes never went silent. Fix: confirm on the GBIS **vehicle-id roll** (the tracked bus leaving the feed is an arrival even when a new bus immediately takes the slot), require 2 silent polls for the silence path (a 1-poll blip where the bus reappears is not an arrival), and widen the predicted window from ≤90 s to ≤240 s — GBIS ETAs are ~60 s-quantized (observed jumps like 137→14→893), so the 90 s window was skipped by most buses (route 9409: 0 predicted events in 3 days).
- **Schedule estimates that lied**: the headway model happily claimed `~2m` at 11 pm when no bus existed. Rule: only show estimates outside the live-reporting window; never say SOON in blue.
- **The learner made it on the board**: the "future learned fallback" got built. The Python prototype moved into a portable C module (`src/learner/`), golden-tested against the prototype on a week of logged arrivals (byte-identical output), and the rings are rebuilt from the buslog in PSRAM at boot. Embedded gotchas: the main task and LVGL task stacks had to grow to 8 KB (ring rebuild + TLS handshakes blew the defaults), and learner buffers moved to PSRAM after TLS allocation starved the heap. Field tuning: the learned-fill window started at 15 min and was dropped to **5 min** after a morning of real buses being hidden behind it — while a Kakao-scheduled 4-minute phantom (a bus the ring had never witnessed) was correctly refused. The learner only claims what it has seen arrive.

## 4. Getting it up and running

Prerequisites: PlatformIO (ESP-IDF 5.x toolchain), a free data.go.kr account.

### 4.1 Register the APIs (all on data.go.kr, same service key)

| Purpose | Service to register | Endpoint used | Notes |
|---|---|---|---|
| Arrivals (primary) | 경기도_버스도착정보 조회 (`6410000`) | `busarrivalservice/v2/getBusArrivalListv2` | Gyeonggi only |
| Arrivals (fallback) | 국토교통부_(TAGO)_버스도착정보 (`1613000`) | `ArvlInfoInqireService/getSttnAcctoArvlPrearngeInfoList` | covers 서울/경기; fills routes GBIS reports empty |
| Air quality | 한국환경공단_대기오염정보 조회 서비스 (`1062168`) | `B552584/ArpltnInforInqireSvc/getMsrstnAcctoRltmMesureDnsty` (+ `getCtprvnRltmMesureDnsty` fallback) | header line, nearest airkorea station |
| Weather | 기상청_단기예보((구)_동네예보) 조회서비스 (`1360000`) | `VilageFcstInfoService_2.0/getVilageFcst` — **note the dot in `_2.0`** | header line, 5 km grid |
| Holidays | 한국천문연구원_특일 정보 (`B090041`) | `SpcdeInfoService/getRestDeInfo` | 공휴일 incl. 대체/임시공휴일; holiday dates run weekend schedules |

Each service needs its own 활용신청 on data.go.kr (the key only works for services that show "승인"). All auto-approve within minutes except the 기상청 one, which can take hours.

### 4.2 Configure the board

1. WiFi + API key:
   ```bash
   cd espidf
   cp src/secret_config.h.example src/secret_config.h   # WiFi + API key
   ```
2. Point it at your stop (in `src/bus.c`): `NODE_ID`/`GBIS_STATION_ID`/`STOP_LABEL`, plus per-board knobs — `ENV_STATION` (nearest airkorea monitoring station), `ENV_SIDO` (fallback), `WX_GRID_X`/`WX_GRID_Y` (기상청 forecast grid for your stop — see §4.3), and the route table `ROWS[]` (headways, first/last bus, ride time). To find your GBIS station ID: `https://m.gbis.go.kr/api/stationSearch?keyword=<stop name>`.
3. Build and flash:
   ```bash
   pio run -t upload    # app-only flash preserves the NVS arrival log
   ```
4. Watch logs: `pio device monitor` (arrivals log as `logged arrival: route 32 at 18:45 (confirmed/predicted)`, env/holiday fetches log their status).

### 4.3 Computing the weather grid for your stop

`WX_GRID_X`/`WX_GRID_Y` are the 기상청 단기예보 5 km grid coordinates (Vilage projection: `RE=6371.00877, GRID=5.0, SLAT1=30, SLAT2=60, OLAT=38, OLON=126, XO=42, YO=135`). E.g. 판교역 = (58, 146). The reference conversion is in `espidf/src/bus.c` (`WX_GRID` config) — or fetch weather for a candidate grid and compare with a phone weather app. If the temp on the header looks like the wrong neighborhood, nudge the grid by the offset.

### 4.4 Optional: data analysis tools

`tools/` has a full offline toolchain (see §2):
- `parse_nvs.py` — decode NVS dumps into per-day arrival events
- `analyze_buslog.py` — weekday arrival-time statistics + prediction accuracy
- `learn_schedule.py` — the learned-schedule prototype (ring model, anomaly detection, re-baseline)
- `host_test.c` + `test_learner.py` — golden test: compile `learner.c` for the host, replay the buslog, assert the C output matches the Python prototype exactly (`cc -o /tmp/lt tools/host_test.c espidf/src/learner/learner.c`)
- `capture_log.py` — headless arrival logger (same detection logic as the firmware, no board needed)

The MicroPython prototype lives in `device/` (copy `config.py.example` → `config.py`).

## 5. Credits

This project is licensed under the **BSD 3-Clause License** — if you build on it, keep the attribution.

- **Board**: JC3248W535EN 320×480 QSPI display module ([vendor listing](https://s.click.aliexpress.com/e/_DFO5uIV)); vendor demo code kept under `espidf/JC3248W535EN/` (not committed)
- **[LVGL](https://github.com/lvgl/lvgl)** — vendored under `espidf/libraries/lvgl`
- **[straga/micropython_lcd](https://github.com/straga/micropython_lcd)** (Kevin G. Schlosser) — MicroPython prototype display drivers
- **[lvgl/lv_micropython](https://github.com/lvgl/lv_micropython)** — prototype firmware image
- **Data**: 경기도 버스정보시스템 (GBIS), 국토교통부 버스도착정보 (TAGO), 한국환경공단 대기오염정보, 기상청 단기예보, 한국천문연구원 특일 정보 — all via data.go.kr
