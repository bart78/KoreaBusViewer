# KoreaBusViewer

An ESP32-S3 signboard for a single Korean bus stop that shows **live arrival countdowns** and — when the live feed goes silent — **learned arrival predictions** reconstructed from the board's own observation log, with honesty gates at every layer.

## 1. TL;DR

- One board, one stop (STOP 07593, Seongnam/Gyeonggi), seven routes (32, 73, 310, 340, 4103, 9409, 9507). Live data from two government APIs (GBIS + TAGO), merged; a static schedule and an **on-device learner** as fallbacks.
- The board logs every arrival it witnesses to flash (60 days). At boot, a portable, dependency-free C learner rebuilds per-route **arrival rings** from that log and, when the feed is silent, shows learned predictions — **only when confident, otherwise an honest `--`**.
- Held-out validation (leave-one-out across 9 full days, 2,995 logged arrivals): the learned ring beats the static headway model on every metric — **median 4 vs 5 min, 59% vs 53% within ±5 min**. With **per-slot quality gating**, the claims the board actually shows land at **median 2 min, 81% within ±5 min** — the loose slots withhold themselves instead of eroding trust.
- Everything — logging, learning, display, honesty rules — runs on the board. No server, no account, no phone required.

## 2. Background & Motivation

**Fixed schedules lie.** A headway model happily claims a bus that doesn't exist (an 11pm `~2m` on a route whose last bus left hours ago) and knows nothing about delays, thinning service, or holidays.

**Live apps are accurate when the feed is fresh — and wrong otherwise.** They display phantom buses (a scheduled 4-minute bus that never came — observed), inherit the feed's blind spots (buses near the route origin invisible until they are close; a second-bus slot that quoted **267 minutes** on a 25-minute-headway route because the feed lost track of intermediate vehicles), and show nothing useful when the feed itself is silent.

The board's problem, stated plainly: *when the feed says nothing, what should the sign say?* "No bus" is wrong. A schedule is a guess. The answer this project settled on: **say what the board has observed**, and say nothing when it hasn't observed enough.

## 3. Hardware

- **Module**: JC3248W535EN — ESP32-S3, 320×480 QSPI display, 16 MB flash.
- **Firmware**: ESP-IDF 5.x + LVGL 8.3, built with PlatformIO (`espidf/`). Single main module `src/bus.c` (~1,700 lines) plus the portable learner `src/learner/`.
- **Storage**: 128 KB NVS partition for the arrival log (60 days at ~2 KB/day).

## 4. Design & Implementation

### 4.1 Data sources (all data.go.kr, one service key)

| Purpose | Service | Notes |
|---|---|---|
| Arrivals (primary) | GBIS 경기도_버스도착정보 `6410000` | `predictTime1/2` per route; vehicle IDs |
| Arrivals (fallback) | TAGO 국토교통부_버스도착정보 `1613000` | covers routes GBIS reports empty |
| Air quality | 한국환경공단 대기오염정보 `1062168` | nearest station + province-median fallback |
| Weather | 기상청 단기예보 `1360000` | endpoint `VilageFcstInfoService_2.0` — **note the dot**; plus 초단기실황 nowcast for the current-hour override |
| Holidays | 한국천문연구원 특일정보 `B090041` | weekend rings include 공휴일 |

**Adaptive cadence** (GBIS+TAGO share a 1000-calls/day budget each): 45 s during the 07–10 rush, 120 s by day, 180 s in the evening, no polling 23:00–05:00, and a 30 s burst while a bus is within 30 s of the stop.

### 4.2 Display semantics

| What you see | Meaning |
|---|---|
| White `4m 10s` | Live countdown (yellow/orange as the feed ages) |
| `ARRIVING` / `JUST LEFT` | Green 20 s / dark-red 20 s acknowledgment of a pass |
| Blue `27m 30s` | **Learned** prediction (from the ring; counts down, never says SOON) |
| Blue `~2Xm` | Static schedule estimate (cold-start fallback) |
| Gray `--` | Nothing claimed — live silence inside the window, or a gated prediction |
| Subtext `+9m` | Second bus: gray = feed's second slot, blue = learned next, `--` = unknown |
| Confidence dot | Green = live data · tier colors = a learned claim's confidence · neutral = static · dim = nothing |

### 4.3 The arrival logger

Every arrival is appended to NVS as a **2-byte event**: `holiday(1)<<15 | type(1)<<14 | route(3)<<11 | minute_of_day(11)` — one blob per day. Detection:

- **Predicted** (type 0): first sighting of a bus within 240 s, deduped per vehicle ID.
- **Confirmed** (type 1): the tracked bus passed — detected by the GBIS **vehicle-id roll**, or **2 consecutive silent polls** (one-poll blips don't count), or a dead-feed release (logged only if the route is still silent when the feed returns — cancelled otherwise, so a reappearing bus never double-counts).
- All paths are mutually exclusive and deduped by vehicle ID within 10 minutes.

### 4.4 The honesty layer

Observed failures, each now clamped:

- The feed's second-bus slot quoting 267 m on a 25-min route → **plausibility clamp**: the second bus must be within 3× the current headway and inside the service window.
- A learned subtext quoting a bus 158 m out (ring hole) → learned claims must be **within 90 minutes**.
- A forecast claiming rain over a clear sky → the header shows the **current** forecast slot (not the next one) and the **observed** nowcast PTY overrides the forecast's.
- `--` instead of a phantom: learned fills render only with confidence ≥ 0.60 and 5–90 min out; the static model's `--` window matches (5 min); the subtext shows `--` only when the main row is not live.

## 5. Algorithm Deep Dive

### 5.1 The ring model

Per route × day-type, the learner keeps the last 10 days of **confirmed arrival minutes**. Days are bucketed as **weekday** vs **weekend+holiday** (the holiday flag rides in the log event itself). The current day is excluded until the night transition, when it is learned as a complete day.

### 5.2 Alignment, slots, and anomalies

1. **Alignment**: each day's arrivals are aligned ordinally against the longest day (a missed bus shifts every later arrival — raw columns would smear slots across the day). A day whose pattern is consistently shifted (median deviation ≥ 8 min, agreed by ≥ 60% of slots) or too incomplete (< 60% of the median day length) is flagged **anomalous** and excluded from scoring.
2. **Slots**: aligned columns are merged within ±6 min; each slot's **median** is its prediction. A slot needs **≥ 3 samples** (days) to be claimable.
3. **Confidence**: the fraction of (day × slot) samples landing within **±3 min** of their slot median, days with no near-arrival skipped — so occasional missed buses don't punish the route. This is a **tightness** metric, not an accuracy one (see §6).

### 5.3 Queries and gates

- `next(now)` → first slot after now with n ≥ 3; `next2` → the slot after it. O(slots), computed per display tick.
- Display priority: **live data always wins** (structurally — the learned branch is unreachable when the feed reports the route). Otherwise:
  - learned **fill** when route confidence ≥ 0.60 **and the claimed slot's own quality ≥ 0.60** and the slot is **5–90 min** out (inside 5 min, live silence reads as unknown → `--`);
  - learned **subtext** (next2) under the same two gates and ≤ 90 min out;
  - static schedule otherwise; `--` when nothing can be claimed.
- **Per-slot quality**: each slot carries its own tightness (fraction of its days' arrivals within ±3 min of the median, same rule as the route confidence). A route average hides a loose 3 pm slot behind a tight 7 am one — so each slot must clear its own gate to claim, and the confidence dot grades a claim by that slot's quality, not the route's.

### 5.4 Embedded constraints

- The learner is portable C (`src/learner/`, no FreeRTOS/LVGL deps) and is **golden-tested**: `tools/test_learner.py` asserts the C output equals the Python prototype exactly, on a week of real log data.
- Rings rebuild at boot into **PSRAM** (the TLS heap starved without it); the main task and LVGL task stacks were raised to 8 KB each (rebuild + TLS handshakes blew the defaults).

## 6. Results

### 6.1 Held-out validation (leave-one-out, 3 full days, 953 arrivals)

For each of three held-out weekdays, the ring was rebuilt from the other six weekdays and scored against that day's logged arrivals — the same exclusion rule the board itself uses (the current day never enters its own ring).

| | Learned ring | Static headway model |
|---|---|---|
| Median error | **4 min** | 5 min |
| ≤ ±3 min | **46%** | 34% |
| ≤ ±5 min | **61%** | 53% |
| ≤ ±10 min | 84% | 84% |

Per held-out day: 08/24 median 4 min (65% ≤5m) · 08/25 median 4 min (63%) · 08/26 median 5 min (55%).

The trajectory matters as much as the snapshot: with only 4 ring days, the static model's dense grid won the raw average (median 3 vs 5 min). At 6–7 ring days, the learned ring leads on every metric. Meanwhile the static model's error is bounded by half a headway *by construction* and it cannot improve — the ring improves with every day it lives.

Per-route confidence after 9 weekday days: 32 → 0.80, 73 → 0.80, 310 → 0.76, 340 → 0.87, 4103 → 0.81, 9409 → 0.66, 9507 → 0.86.

**Slot-level gating (per-slot quality ≥ 0.60)** is the largest single accuracy lever: on the same 2,995 held-out arrivals, claims that pass the slot gate land at **median 2 min, 66% ≤3 min, 81% ≤5 min** (n=1,658) versus 4 min / 44% / 59% for all slots. Withholding the loose slots costs ~45% of claims and buys a 2× median-error improvement on what remains.

### 6.2 Field observations (live, one morning)

- The learned **10:53** estimate for route 340 landed while the feed was silent; the bus arrived on schedule.
- A Kakao-scheduled **4-minute phantom was refused** (`--` shown); it never came.
- The ring's **11:18** slot was confirmed by live data to the minute, and one bus ran 5 minutes late — the ring absorbed the variance.
- The feed's **267 m second-bus lie** was clamped; a later "materializing" 12-minute bus was arbitrated by the ring and the log: the steady `+22m` subtext was the 18:39 slot's bus (arrived 18:38), and the 12 m bus was the 18:18 slot's bus (arrived 18:20) — *not* a hidden bus, the feed was just late to report an on-schedule bus.

### 6.3 Capture audit

The ring inherits feed coverage. Route 310 logged only **10 confirmed arrivals in a full day against 129 predicted sightings** — its confirm path (vehicle-id roll / 2 silent polls) rarely satisfies, while the other routes log 26–64. This is the mechanism behind every hole in the ring, and the reason the confidence metric (tightness, not coverage) can stay high on a holey route — see §7.

## 7. Future Work

- **Coverage-aware route confidence** — the slot gate handles loose slots, but the route-level confidence still can't see coverage: route 310 scores 0.76 while its capture stays sparse (its days genuinely lack most arrivals, so slots have few samples to be tight about). Weight route confidence by the share of the service window with claimable slots.
- **Fix 310's confirm path** — its predicted sightings fire often but the confirm conditions rarely do: the "2 silent polls" rule was a time threshold in disguise, and the slower polling cadence (45/120/180 s) silently stretched it to 4–6 min. **Done (Aug 31)**: the silence rule is now time-based (90 s of absence, poll-count-independent) and an id-less ETA-jump confirm (a close bus whose slot jumps >8 min outward, persisting one poll) covers continuously-reported feeds. **Extended (Sep 5)**: manual-tap cross-checking exposed the deeper systematic gap — feeds with coarse ETA updates (5-min quantization) never enter the 240 s close window before a pass, so whole trips were displayed but unconfirmable (route 4103's 07:30 trip: 80% of taps missed). The roll and jump paths now track buses up to 8 min out (the ID roll is decisive; the jump keeps its persistence).
- **Permanent held-out harness** — validate every night's ring against the next day automatically, in the tools, so each firmware change ships with a regression number.
- **Manual-tap validation** — a companion tap app quantifies the miss rate per route (how many physical buses the feed never reports), turning feed blind spots into a measured number.
- **Per-route gate tuning** — ride-time-aware windows (a route whose origin is 12 min away has a different honest window than a 40-min route).
- **The commute planner** — the ring generalizes to multiple stops/legs; a planner would optimize best-leave-time over walk + bus + subway distributions. (Separate project.)

## 8. Getting Started

1. Register the five data.go.kr services (§4.1, same key) — all auto-approve within minutes except 기상청.
2. `cp espidf/src/secret_config.h.example espidf/src/secret_config.h` (WiFi + key); point `NODE_ID`/`GBIS_STATION_ID`/`STOP_LABEL`, `ENV_STATION`, `WX_GRID_X/Y`, and the `ROWS[]` table at your stop. Find the station ID via `https://m.gbis.go.kr/api/stationSearch?keyword=<stop name>`.
3. `cd espidf && pio run -t upload` (app-only flash preserves the NVS log).
4. Watch logs with `pio device monitor`.

Tools (offline analysis): `parse_nvs.py` (decode NVS dumps), `analyze_buslog.py` (arrival statistics), `learn_schedule.py` (the Python prototype the C is golden-tested against), `host_test.c` + `test_learner.py` (the golden test), `capture_log.py` (headless logger).

## 9. Credits & License

BSD 3-Clause — attribution required. Board: JC3248W535EN module; display drivers per the vendor demo. Data: 경기도 버스정보시스템, 국토교통부 (TAGO), 한국환경공단, 기상청, 한국천문연구원 — via data.go.kr.
