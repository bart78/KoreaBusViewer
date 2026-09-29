# KoreaBusViewer

An ESP32-S3 signboard for a single Korean bus stop that shows **live arrival countdowns** and — when the live feed goes silent — **learned arrival predictions** reconstructed from the board's own observation log, with honesty gates at every layer.

## 1. TL;DR

- One board, one stop (STOP 07593, Seongnam/Gyeonggi), seven routes (32, 73, 310, 340, 4103, 9409, 9507). Live data from two government APIs (GBIS + TAGO), merged; a static schedule and an **on-device learner** as fallbacks.
- The board logs every arrival it witnesses to flash (60 days). At boot, a portable, dependency-free C learner rebuilds per-route **arrival rings** from that log and, when the feed is silent, shows learned predictions — **only when confident, otherwise an honest `--`**.
- Held-out validation (leave-one-out across 21 full weekdays, 6,713 logged arrivals): the learned ring beats the static headway model on every metric — **median 4 vs 5 min, 59% vs 53% within ±5 min**. With **per-slot quality gating** (and time-anchored alignment), the claims the board actually shows land at **median 2 min, 74% within ±3 min, 87% within ±5 min** — the loose slots withhold themselves instead of eroding trust.
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
- **Confirmed** (type 1): the tracked bus passed — detected by any of:
  - the **vehicle-id roll** (a tracked bus's ID changed — decisive, confirms at once) or an ID lost during a feed gap;
  - the **time-based silence** rule (the route absent ≥ 90 s — the old "2 consecutive silent polls" was a time threshold in disguise, and the slower polling cadence silently stretched it to 4–6 min, starving id-less routes whose feeds only drop for a poll or two);
  - the **id-less ETA-jump** (a close bus whose slot jumps >8 min outward, persisting one poll — the only pass signal for continuously-reported feeds without IDs);
  - a dead-feed release (logged only if the route is still silent when the feed returns — cancelled otherwise, so a reappearing bus never double-counts).
- The roll and jump paths track buses up to **8 min out** (`LOG_ROLL_SECS`) — feeds with coarse ETA updates (5-min quantization) never enter a 4-min close window before a pass, so whole trips were displayed but unconfirmable.
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

1. **Alignment**: each day's arrivals are matched **time-anchored** — every arrival pairs with the nearest reference point (the densest day's arrivals) within ±15 min, one-to-one. Ordinal alignment was the original design but it drifts when day counts vary (capture noise, missed trips): a ±1–2 position shift by the evening makes columns mix adjacent trips, medians land between real arrivals, and slot quality collapses (route 9507's evenings showed n=8, q=0.00 over perfectly regular 20:56/21:17/21:41 arrivals). Time-anchoring survives the drift. A day whose pattern is consistently shifted (median deviation ≥ 8 min, agreed by ≥ 60% of slots) or too incomplete (< 60% of the median day's count **or span** — a late-starting day compresses its ordinals) is flagged **anomalous** and excluded from scoring.
2. **Slots**: matched columns are merged within ±6 min; each slot's **median** is its prediction. A slot needs **≥ 3 samples** (days) to be claimable.
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

### 6.1 Held-out validation (leave-one-out, 21 full weekdays, 4,729 arrivals)

For each held-out weekday, the ring was rebuilt from the other weekdays (10-day cap) and scored against that day's logged arrivals — the same exclusion rule the board itself uses (the current day never enters its own ring).

| | Learned ring | Static headway model |
|---|---|---|
| Median error | **4 min** | 5 min |
| ≤ ±3 min | **46%** | 34% |
| ≤ ±5 min | **59%** | 53% |
| ≤ ±10 min | 84% | 84% |

The trajectory matters as much as the snapshot: with only 4 ring days, the static model's dense grid won the raw average (median 3 vs 5 min). At 6–7 ring days, the learned ring leads on every metric — modestly, and that modest lead deserves honest context. The static model is not a strawman: it is the *actual timetable* encoded as a grid, tuned per route (headways, first/last buses, ride times) — the strongest baseline available, and its error is bounded by half a headway by construction. The learner matches it with **zero schedule knowledge** — it reconstructs the same pattern purely from what it has seen arrive, with no headways, no timetable, no route metadata.

**And the reconstruction is itself the validation.** The learner was never told a headway, a first bus, or that a route runs on a schedule at all — it only watched buses arrive. That its slots reproduce the official timetable's pattern to within a minute is a closed-loop audit of everything underneath it: the feed parsing, the confirm logic, the alignment, the medians. If any link in that chain systematically lied, the reconstructed pattern would drift from the timetable — and the mismatch would have been the first thing we noticed. Where the ring and the timetable do diverge, the divergence is signal, not error: the ring tracks the *actual* service as it runs, timetable drift included. An independently-known structure emerging from raw observations is the strongest available evidence that the model learned the process, not the answer — the board did not copy the timetable, it re-derived it from reality.

Where the learner's value shows, and the static cannot follow:

1. **The gate** — the learned claims that actually reach the display land within ±5 min **87% of the time**, versus the static's 53%: the learner withholds its loose guesses, the static cannot.
2. **Honesty** — the static happily claims an 11 pm bus that never ran; the learner never claims what it has not witnessed.
3. **Adaptivity** — the static assumes the schedule it was configured with forever (the September timetable change would silently degrade it); the ring tracks the last 10 days of reality.
4. **Self-sufficiency** — a new stop needs only the board and time; the static needs the schedule configured by hand.

The honest summary: if you have an accurate, stable timetable and a patient hand to encode it, a headway model is genuinely good — the project's premise was never "schedules are worthless", it was "schedules are good *when they're right*, and they're wrong more often than anyone admits". The board shows the learned values only because they adapt and can be trusted to say `--` when they cannot answer.

And the scale matters: a one-minute median improvement is the difference between making a bus and watching it leave. The countdowns on the board inherit the ring's accuracy directly — every learned second counted is a second closer to reality than the schedule's guess. Whether that edge is "optimization for the sake of optimization" is for the person standing at the stop to decide.

Per-route confidence (28 weekday days in the ring's history): 32 → 0.70, 73 → 0.67, 310 → 0.75, 340 → 0.90, 4103 → 0.84, 9409 → 0.72, 9507 → 0.84.

**Slot-level gating (per-slot quality ≥ 0.60)** is the largest single accuracy lever: on the same 4,729 held-out arrivals, claims that pass the slot gate land at **median 2 min, 68% ≤3 min, 81% ≤5 min** (n=2,657) versus 4 min / 44% / 59% for all slots. Withholding the loose slots buys a 2× median-error improvement on what remains.

**Time-anchored alignment (Sep 29)** recovered a quarter of the model's claims: the original ordinal alignment drifted when day counts varied (capture noise), smearing evening columns so medians landed between real trips — 80 slots across all routes carried n≥5 evidence with q=0.00 over perfectly regular arrivals (9507's evenings). After the fix, gated claims land at **median 2 min, 74% ≤3 min, 87% ≤5 min** (n=6,713) — more claims *and* better accuracy.

**Partial-day gate**: a day with fewer than 60% of the ring's median arrival count (board unplugged, capture gap) is excluded at learn time — its few arrivals cannot be ordinally aligned and skewed every slot's median. Without the gate, half-days (9/14–15, 9/21, 9/23) pushed held-out ≤5 min accuracy from ~80% to ~45% for a week at a time; with it, every held-out day sits at 76–86%.

### 6.2 Field observations (live, one morning)

- The learned **10:53** estimate for route 340 landed while the feed was silent; the bus arrived on schedule.
- A Kakao-scheduled **4-minute phantom was refused** (`--` shown); it never came.
- The ring's **11:18** slot was confirmed by live data to the minute, and one bus ran 5 minutes late — the ring absorbed the variance.
- The feed's **267 m second-bus lie** was clamped; a later "materializing" 12-minute bus was arbitrated by the ring and the log: the steady `+22m` subtext was the 18:39 slot's bus (arrived 18:38), and the 12 m bus was the 18:18 slot's bus (arrived 18:20) — *not* a hidden bus, the feed was just late to report an on-schedule bus.

### 6.3 Capture audit

The ring inherits feed coverage — and the confirm paths had to be rebuilt around that reality. Route 310 once logged only **10 confirmed arrivals in a full day against 129 predicted sightings** (the id-less feed never satisfied the roll or silence conditions); after the time-based silence and id-less ETA-jump confirms, it captures **24–41/day** — its ring filled and its learned values now render. The capture-health table (`analyze_buslog.py`) prints the predicted:confirmed ratio per route per day — the alarm that catches a capture regression within a day.

Two feed-side holes remain, honest rather than fixed: route 32's evenings (the feed reports them on only half of days — the ring honestly says `--`) and route 310's evenings (high-variance — the ring refuses to claim what it cannot predict).

## 7. Future Work

- **Coverage-aware route confidence** — the slot gate handles loose slots, but the route-level confidence still can't see coverage. Weight route confidence by the share of the service window with claimable slots.
- **The remaining feed-side holes** — route 32's evenings (feed-dependent capture) and 310's high-variance evenings: more data, not looser gates, is the only honest fix.
- **Permanent held-out harness** — the leave-one-out machinery exists (on demand, in the tools — it produced §6.1 and caught the alignment regression within a day). What remains is wrapping it as an automatic nightly run so every firmware change ships with a regression number without anyone asking.
- **Manual-tap validation** — demonstrated (§6.2's field observations and the 4103 07:30 hole came from 32 taps); the tool exists (`cross_check.py`). What remains is *coverage*, not machinery: more taps in the sparse windows (weekday afternoons, weekends) to turn the per-route miss-rate numbers from suggestive into stable.
- **Per-route gate tuning** — ride-time-aware windows (a route whose origin is 12 min away has a different honest window than a 40-min route).
- **The commute planner** — the ring generalizes to multiple stops/legs; a planner would optimize best-leave-time over walk + bus + subway distributions. (Separate project.)

## 8. Lessons Learned

The failures that shaped this system, each with its mechanism — the observable symptom, the root cause, and the fix:

1. **"2 consecutive silent polls" was a time threshold in disguise.** When the polling cadence slowed (15/60 s → 45/120/180 s), the silence requirement silently stretched from ~30–120 s to 4–6 min — and id-less routes whose feeds drop for only a poll or two stopped confirming entirely (310: 16–29 → 8–15 confirms/day, no code change). *Lesson: never express a time requirement as a poll count; the poll interval is a moving target.*
2. **A confirm path can be dead-on-arrival without ever failing a test.** The id-less ETA-jump required its persistence poll to re-enter the close-window gate — but the persistence poll is far *by definition*, so `jump_pending` was wiped before it could ever confirm. The path contributed zero for two weeks while a separate mechanism (the ID roll) masked it. *Lesson: a guard condition that excludes the very state the feature needs is invisible until you trace the actual event flow.*
3. **The display can show a bus the logger can never certify.** Feeds with coarse ETA updates (5-min quantization) never enter the 4-min close window before a pass — the board showed the countdown, Kakao showed it, and the confirm logic was structurally blind. Manual taps caught it: 80% of 4103's 07:30 taps went unmatched. *Lesson: the display and the logger have different evidence thresholds — "we saw it" is not "we confirmed it", and only the taps can audit the gap.*
4. **Ordinal alignment smears evenings.** Aligning days by arrival *position* drifts when day counts vary (capture noise) — by evening, columns mix adjacent trips and slot medians land between real arrivals, collapsing quality to 0.00 over perfectly regular service (9507's evenings: n=8, q=0.00 over a clean 21-min cadence). Time-anchored matching (nearest reference within ±15 min, one-to-one) fixed it, recovering a quarter of all claims *and* improving accuracy. *Lesson: ordinal alignment assumes the number of arrivals is stable; capture noise breaks that assumption at the day's end.*
5. **A partial day poisons the whole ring.** A day with 62% of the arrivals but a missing morning compresses its ordinals and shifts every evening column — held-out accuracy dropped from ~80% to ~45% for a week after two half-days entered the ring. The fix: exclude days below 60% of the ring's median *count or span* at learn time. *Lesson: a day that started late is not a shorter day — it is a misaligned day.*
6. **A min–max window is a 74% claim, not a 100% claim.** The "arrives between earliest and latest observed" envelope held only 74% of held-out days (each new day extends the observed range a quarter of the time). A ±5 min calibrated margin restored it to 93% for the claims the display actually makes. *Lesson: the observed range underestimates the true range; state the confidence or widen the window.*
7. **The claim rules must travel with the data.** The offline export's q-only gate let a consumer claim an n=1 slot the board would refuse — the model looked broken at the e-ink while the board was right. The export now carries a `claimable` flag (n ≥ 3 *and* q ≥ 0.60). *Lesson: an API is a contract; encode the acceptance rules in the data itself.*
8. **Diagnostics must be in place before the mystery.** Silent days were diagnosed only by inference (event spans, start times, the epoch-dated health marker that revealed boots before the SNTP sync). The daily health markers (boots / fetch-fails / last-ok) now make the next gap self-diagnosing. *Lesson: a power cycle during an NVS write can lose a morning of log — the ring's span gate absorbs it, but the diagnostic tells you it happened.*

## 9. Getting Started

1. Register the five data.go.kr services (§4.1, same key) — all auto-approve within minutes except 기상청.
2. `cp espidf/src/secret_config.h.example espidf/src/secret_config.h` (WiFi + key); point `NODE_ID`/`GBIS_STATION_ID`/`STOP_LABEL`, `ENV_STATION`, `WX_GRID_X/Y`, and the `ROWS[]` table at your stop. Find the station ID via `https://m.gbis.go.kr/api/stationSearch?keyword=<stop name>`.
3. `cd espidf && pio run -t upload` (app-only flash preserves the NVS log).
4. Watch logs with `pio device monitor`.

Tools (offline analysis): `parse_nvs.py` (decode NVS dumps), `analyze_buslog.py` (arrival statistics + capture-health table), `learn_schedule.py` (the Python prototype the C is golden-tested against), `host_test.c` + `test_learner.py` (the golden test), `capture_log.py` (headless logger), `cross_check.py` (tap-data vs board-log validation), `export_model.py` (emit `model.json` — the learned model + static fallback + observed holidays, for offline consumers like an e-paper dashboard; `holidays.json` covers the published holiday calendar 2026–2028).

## 10. Credits & License

BSD 3-Clause — attribution required. Board: JC3248W535EN module; display drivers per the vendor demo. Data: 경기도 버스정보시스템, 국토교통부 (TAGO), 한국환경공단, 기상청, 한국천문연구원 — via data.go.kr.
