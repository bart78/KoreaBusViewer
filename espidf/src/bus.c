#include <lvgl.h>
#include "esp_bsp.h"
#include "lv_port.h"
#include "display.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_sntp.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <stdio.h>
#include <math.h>
#include <netdb.h>
#include <arpa/inet.h>
#include "nvs.h"
#include "../lv_font_mono28.c"
#include "learner/learner.h"

static const char* TAG = "BUS";

// ---- config ----
#include "secret_config.h"

#ifndef WIFI_SSID
#error "src/secret_config.h missing: copy secret_config.h.example and fill in your values"
#endif
#define CITY_CODE     "31020"
#define NODE_ID       "GGB206000648"
#define GBIS_STATION_ID "206000648"
#define STOP_LABEL    "STOP 07593"

// adaptive polling cadence. Budget: ~800 cycles/day per feed on weekdays
// (one cycle = one GBIS + one TAGO call) vs the 1000/day data.go.kr cap.
//  - 05:00-07:00   150s   early service, no rush
//  - 07:00-10:00    45s   weekday rush (leave-home window)
//  - 10:00-20:00   120s   regular day
//  - 20:00-23:00   180s   evening wind-down (decoration mode)
//  - 23:00-05:00     off   schedule display only, zero calls
//  - any bus <=30s  30s   close-bus burst: ARRIVING confirmed within ~30s
#define CAD_START_HOUR  5      // fallback wake if no route has schedule data
#define WAKE_MINS       30     // display + polling resume this long before first bus
#define CAD_RUSH_START  7
#define CAD_RUSH_END    10
#define CAD_EVE_START   20
#define CAD_NIGHT_END   23
#define CAD_MORN_SECS   150
#define CAD_RUSH_SECS   45
#define CAD_DAY_SECS    120
#define CAD_EVE_SECS    180
#define CAD_OFF_SECS    300   // re-check the window while not polling
#define CLOSE_WIN_SECS  30
#define CLOSE_REFRESH   30
#define RETRY_REFRESH   20

// optional header env line (air quality + weather). data.go.kr services
// 1062168 (한국환경공단 대기오염정보) and 1360000 (기상청 단기예보) must be
// registered on the API key; the line stays hidden until first success.
#define ENV_REFRESH_SECS  600
#define ENV_STATION       "대왕판교로(백현동)"  // nearest airkorea station
#define ENV_SIDO          "경기"               // fallback: province median
#define WX_GRID_X         58                   // Vilage forecast grid, 판교역
#define WX_GRID_Y         146

// arrival capture windows (seconds). GBIS ETAs are ~60s-quantized, so a
// 90s predicted window was skipped by most buses; 240s matches the
// confirmed window. Buses beyond LOG_REARM_SECS re-arm the predicted dedupe.
#define LOG_PREDICT_SECS  240
#define LOG_CONFIRM_SECS  240
#define LOG_REARM_SECS    300

// how long "ARRIVING" stays on screen after a confirmed arrival before the
// row moves on to the next bus / schedule placeholder
#define ARRIVING_SHOW_SECS 20
// then "JUST LEFT" (dark red) for a bit before the row releases
#define JUST_LEFT_SHOW_SECS 20

// fixed display order: routes always shown, schedule from 성남 노선현황 CSV
typedef struct {
    int number;
    int hw_commute;  // 출퇴근 headway (min)
    int hw_weekday;  // 평일 headway
    int hw_weekend;  // 주말 headway
    int first_hm;    // first bus from origin as minutes (0..1439)
    int last_hm;     // last bus from origin as minutes
    int to_stop;     // estimated ride time origin -> our stop (min)
    int has_sched;
} row_cfg_t;

static const row_cfg_t ROWS[] = {
    {32,    8, 13, 15,  5*60+50, 23*60+20, 15, 1},
    {73,   11, 20, 35,  6*60+30, 20*60+ 0,  8, 1},
    {310,  15, 25, 35,  5*60+ 0, 22*60+30, 20, 1},
    {340,  17, 27, 40,  4*60+40, 22*60+ 0, 12, 1},
    {4103, 15, 40, 40,  5*60+ 0, 23*60+ 0,  8, 1},
    {9409, 25, 25, 27,  5*60+ 0, 23*60+ 0, 15, 1},
    {9507, 19, 33, 52,  4*60+40, 22*60+30,  8, 1},
};
#define N_ROWS (sizeof(ROWS) / sizeof(ROWS[0]))

// per-row live arrival state
typedef struct {
    int has_arrival;
    int arrtime;      // seconds at fetch
    int arrtime2;     // second bus, seconds at fetch (-1 unknown)
    int seats;        // remaining seats on first bus (-1 unknown)
    int veh;          // GBIS vehicle id of first bus (-1 unknown)
    int veh_logged;   // vehicle id of the last logged arrival
    int veh_confirmed; // vehicle id of the last confirmed arrival (-1 none)
    time_t veh_conf_at; // when that confirm fired (dedupe window)
    int logged;       // arrival already logged to NVS
    time_t logged_ts; // arrival moment of the last predicted log
    time_t fetched;
    int disp;         // smoothed seconds shown on screen (-1 = snap to live)
    int target;       // seconds the display should chase toward
    int anim;         // 1 = the 33ms anim timer owns this row's text
    int vel;          // current chase velocity (s/tick, signed)
    int shown;        // last disp value rendered (render-throttle)
    int zombie;       // feed quiet but bus not yet due: keep counting
    int last_eta;     // last live ETA before the row left the live state
    time_t due_at;    // when a dead-feed countdown first exhausted (release anchor)
    time_t arrived_at; // confirmed arrival moment (shows ARRIVING briefly)
    int rel_pending;  // outage arrival awaiting feed confirmation
    time_t rel_due;   // due moment of the pending outage arrival
    // confirmed-arrival tracking: a close bus is gone for good only when
    // the vanish survives (roll / 2 silent polls); state survives polls
    int confirm_pending;  // a close bus vanished and silence is being counted
    int silent_polls;     // consecutive silent polls since the vanish
    int pend_eta;         // last ETA of the vanished bus (arrival timestamp)
    time_t pend_fetch;    // fetch time of that last sighting
} row_state_t;
static row_state_t rows[N_ROWS];

static time_t last_refresh = 0;
static int refresh_secs = 60;
static time_t last_good = 0;
static int fetch_fail = 0;
static int backlight_on = 1;   // display lit during the polling window only

// ---- learned schedule (rings rebuilt from the buslog) ----
// learned schedule — rings live in PSRAM (53KB would starve the internal
// heap that the TLS sessions need)
#include "esp_heap_caps.h"
static learner_t* learners;
static int* learn_conf;
static int* learn_confn;
static int learner_ready;
static int learner_last_day;   // YYYYMMDD of the last day learned

#define LEARNED_FILL_CONF     0.60  // min confidence to use learned fills
#define LEARNED_SUBTEXT_CONF  0.60  // min confidence for the learned subtext
#define LEARNED_FILL_MIN_SECS 300   // learned fills show beyond this (5 min);
                                    // inside it, live silence reads as unknown

// ---- env header state ----
typedef struct {
    int have_pm;
    int pm10;   // ug/m3, -1 unknown
    int pm25;
    int have_wx;
    int temp;   // C, -1 unknown
    int sky;    // KMA sky code 1/3/4, -1 unknown
    int pty;    // KMA precipitation type 0..4, -1 unknown
} env_t;
static env_t env;
static time_t env_next;

// ---- LVGL objects ----
static lv_obj_t* hdr_label;
static lv_obj_t* env_group;
static lv_span_t* env_span[7];
static lv_obj_t* clock_label;
static lv_obj_t* route_labels[N_ROWS];
static lv_obj_t* seats_labels[N_ROWS];
static lv_obj_t* time1_labels[N_ROWS];
static lv_obj_t* time2_labels[N_ROWS];

static time_t kst_now(void) { return time(NULL) + 9 * 3600; }

static void fmt_secs(int s, char* out, size_t n) {
    if (s <= 0) { snprintf(out, n, "ARRIVING"); return; }
    snprintf(out, n, "%02dm %02ds", s / 60, s % 60);
}

// ---- HTTP ----
static char g_body[65536];
static int g_body_len;

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data_len > 0 && g_body_len + evt->data_len < (int)sizeof(g_body)) {
            memcpy(g_body + g_body_len, evt->data, evt->data_len);
            g_body_len += evt->data_len;
        }
    }
    return ESP_OK;
}

// extract a numeric field within [start, end) — order-independent, quoted or not
static long item_num(const char* start, const char* end, const char* key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char* p = start;
    while ((p = strstr(p, pat)) != NULL && p < end) {
        p += strlen(pat);
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        if (p < end && *p == '"') {
            p++;
            long v = 0;
            int has = 0;
            while (p < end && *p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; has = 1; }
            return has ? v : -1;
        }
        if (p < end && *p == 'n') return -1;  // null
        return atol(p);
    }
    return -1;
}

static int item_str(const char* start, const char* end, const char* key, char* out, size_t n) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(start, pat);
    if (!p || p >= end || n == 0) return 0;
    p += strlen(pat);
    size_t len = 0;
    while (p + len < end && p[len] != '"' && len + 1 < n) len++;
    memcpy(out, p, len);
    out[len] = 0;
    return 1;
}

static char* json_str(const char* json, const char* key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char* p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    const char* e = strchr(p, '"');
    if (!e) return NULL;
    int len = e - p;
    char* out = malloc(len + 1);
    memcpy(out, p, len);
    out[len] = 0;
    return out;
}

// pending arrivals merged from both feeds (deduped per route: keep earliest)
static int pend_no[32];
static int pend_at[32];
static int pend_at2[32];
static int pend_seats[32];
static int pend_veh[32];
static int pend_n;

static void pend_add(int no, int at, int at2, int seats, int veh) {
    for (int i = 0; i < pend_n; i++) {
        if (pend_no[i] == no) {
            if (at < pend_at[i]) {
                pend_at[i] = at;
                pend_veh[i] = veh;
            }
            if (at2 >= 0 && at2 < pend_at2[i]) pend_at2[i] = at2;
            if (seats > pend_seats[i]) pend_seats[i] = seats;
            return;
        }
    }
    if (pend_n < 32) {
        pend_no[pend_n] = no;
        pend_at[pend_n] = at;
        pend_at2[pend_n] = at2;
        pend_seats[pend_n] = seats;
        pend_veh[pend_n] = veh;
        pend_n++;
    }
}

static int http_get(const char* url) {
    g_body_len = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 32768,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return 0;
    esp_err_t perr = esp_http_client_perform(client);
    int st = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (g_body_len > 0) g_body[g_body_len] = 0;
    if (g_body_len >= (int)sizeof(g_body) - 1) {
        ESP_LOGW(TAG, "response truncated (%d bytes)", g_body_len);
    }
    if (perr != ESP_OK || st != 200 || g_body_len == 0)
        ESP_LOGW(TAG, "http fail: %s st=%d len=%d", esp_err_to_name(perr), st, g_body_len);
    return (perr == ESP_OK && st == 200 && g_body_len > 0);
}

// GBIS (경기버스정보시스템) — primary feed, covers all Gyeonggi routes incl. 32/9409
static void log_arrival(int route_idx, time_t arrival_ts, int type);
static int is_holiday(time_t t);

static int fetch_gbis(void) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://apis.data.go.kr/6410000/busarrivalservice/v2/getBusArrivalListv2?serviceKey=%s&stationId=%s&format=json",
             API_KEY, GBIS_STATION_ID);
    if (!http_get(url)) return 0;

    const char* arr = strstr(g_body, "\"busArrivalList\"");
    if (!arr) return 1;  // valid response, no list
    const char* p = strchr(arr, '{');
    while (p) {
        const char* e = strchr(p + 1, '}');
        if (!e) break;
        long no = item_num(p, e, "routeName");
        long at = item_num(p, e, "predictTimeSec1");
        if (no > 0 && at >= 0) {
            char flag[8] = "PASS";
            item_str(p, e, "flag", flag, sizeof(flag));
            if (strcmp(flag, "STOP") != 0 && strcmp(flag, "WAIT") != 0)
                pend_add((int)no, (int)at,
                         (int)item_num(p, e, "predictTimeSec2"),
                         (int)item_num(p, e, "remainSeatCnt1"),
                         (int)item_num(p, e, "vehId1"));
        }
        p = strchr(e + 1, '{');
    }
    return 1;
}

// TAGO (국토교통부 버스도착정보) — fallback for routes GBIS reports empty
static int fetch_tago(void) {
    char url[512];
    snprintf(url, sizeof(url),
             "https://apis.data.go.kr/1613000/ArvlInfoInqireService/getSttnAcctoArvlPrearngeInfoList?serviceKey=%s&cityCode=%s&nodeId=%s&pageNo=1&numOfRows=20&_type=json",
             API_KEY, CITY_CODE, NODE_ID);
    if (!http_get(url)) return 0;

    const char* items = strstr(g_body, "\"itemList\":");
    if (!items) items = strstr(g_body, "\"item\":");
    if (!items) items = strstr(g_body, "\"items\":");
    const char* p = items ? strchr(items, '[') : NULL;
    if (!p) p = items ? strchr(items, '{') : NULL;  // single-item response
    while (p) {
        const char* e = strchr(p + 1, '}');
        if (!e) break;
        long no = item_num(p, e, "routeno");
        long at = item_num(p, e, "arrtime");
        if (no > 0 && at >= 0) pend_add((int)no, (int)at, -1, -1, -1);
        p = strchr(e + 1, '{');
    }
    return 1;
}

static void urlenc(const char* in, char* out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)in; *p && o + 3 < n; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' || *p == '~')
            out[o++] = *p;
        else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
}

static int cmp_int(const void* a, const void* b) { return *(const int*)a - *(const int*)b; }

// air quality + weather for the header env line. Every failure is silent:
// the line keeps its last good values and simply stops refreshing.
static void fetch_env(void) {
    char url[768];
    char enc[64];
    int pm10 = -1, pm25 = -1;

    urlenc(ENV_STATION, enc, sizeof(enc));
    snprintf(url, sizeof(url),
             "https://apis.data.go.kr/B552584/ArpltnInforInqireSvc/"
             "getMsrstnAcctoRltmMesureDnsty?serviceKey=%s&returnType=json&numOfRows=1"
             "&pageNo=1&stationName=%s&dataTerm=DAILY&ver=1.4", API_KEY, enc);
    if (http_get(url)) {
        const char* p = strchr(g_body, '{');
        const char* e = p ? strchr(p + 1, '}') : NULL;
        if (e) {
            pm10 = (int)item_num(p, e, "pm10Value");
            pm25 = (int)item_num(p, e, "pm25Value");
        }
    }
    if (pm10 < 0 && pm25 < 0) {
        // station miss: province-wide median as a fallback
        urlenc(ENV_SIDO, enc, sizeof(enc));
        snprintf(url, sizeof(url),
                 "https://apis.data.go.kr/B552584/ArpltnInforInqireSvc/"
                 "getCtprvnRltmMesureDnsty?serviceKey=%s&returnType=json&numOfRows=200"
                 "&pageNo=1&sidoName=%s&ver=1.4", API_KEY, enc);
        if (http_get(url)) {
            int a[128], b2[128];
            int na = 0, nb = 0;
            const char* p = g_body;
            while (p && (p = strchr(p, '{')) && na < 128) {
                const char* e = strchr(p + 1, '}');
                if (!e) break;
                long v = item_num(p, e, "pm10Value");
                if (v >= 0 && na < 128) a[na++] = (int)v;
                v = item_num(p, e, "pm25Value");
                if (v >= 0 && nb < 128) b2[nb++] = (int)v;
                p = e + 1;
            }
            if (na) { qsort(a, na, sizeof(int), cmp_int); pm10 = a[na / 2]; }
            if (nb) { qsort(b2, nb, sizeof(int), cmp_int); pm25 = b2[nb / 2]; }
        }
    }
    if (pm10 >= 0 || pm25 >= 0) {
        env.pm10 = pm10;
        env.pm25 = pm25;
        env.have_pm = 1;
    }

    // weather: next forecast slot (TMP/SKY/PTY) from the latest published base
    time_t bt = kst_now() - 2 * 3600;
    struct tm b;
    localtime_r(&bt, &b);
    static const int base_slots[] = {2, 5, 8, 11, 14, 17, 20, 23};
    int base_h = 2;
    for (int i = 0; i < 8; i++)
        if (base_slots[i] <= b.tm_hour) base_h = base_slots[i];  // latest published run
    snprintf(url, sizeof(url),
             "https://apis.data.go.kr/1360000/VilageFcstInfoService_2.0/"
             "getVilageFcst?serviceKey=%s&numOfRows=400&pageNo=1"
             "&base_date=%04d%02d%02d&base_time=%02d00&nx=%d&ny=%d&dataType=json",
             API_KEY, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, base_h,
             WX_GRID_X, WX_GRID_Y);
    if (http_get(url)) {
        time_t now = kst_now();
        struct tm tn;
        localtime_r(&now, &tn);
        int today = (tn.tm_year + 1900) * 10000 + (tn.tm_mon + 1) * 100 + tn.tm_mday;
        int want = tn.tm_hour * 100 + tn.tm_min;
        int best_fd = 0, best_ft = 0;
        const char* p = g_body;
        while (p && (p = strchr(p, '{'))) {
            const char* e = strchr(p + 1, '}');
            if (!e) break;
            long fd = item_num(p, e, "fcstDate");
            long ft = item_num(p, e, "fcstTime");
            if (fd > 0 && ft >= 0 && (fd > today || (fd == today && ft >= want))) {
                if (!best_fd || fd < best_fd || (fd == best_fd && ft < best_ft)) {
                    best_fd = (int)fd;
                    best_ft = (int)ft;
                }
            }
            p = e + 1;
        }
        if (best_fd) {
            int tmp = -1, sky = -1, pty = -1;
            p = g_body;
            while (p && (p = strchr(p, '{'))) {
                const char* e = strchr(p + 1, '}');
                if (!e) break;
                if ((int)item_num(p, e, "fcstDate") == best_fd &&
                    (int)item_num(p, e, "fcstTime") == best_ft) {
                    char cat[8];
                    if (item_str(p, e, "category", cat, sizeof(cat))) {
                        long v = item_num(p, e, "fcstValue");
                        if (v >= 0) {
                            if (!strcmp(cat, "TMP")) tmp = (int)v;
                            else if (!strcmp(cat, "SKY")) sky = (int)v;
                            else if (!strcmp(cat, "PTY")) pty = (int)v;
                        }
                    }
                }
                p = e + 1;
            }
            if (tmp >= 0) {
                env.temp = tmp;
                env.sky = sky;
                env.pty = pty;
                env.have_wx = 1;
            }
        }
    }
    if (env.have_pm || env.have_wx) {
        ESP_LOGI(TAG, "env: pm10 %d pm25 %d temp %d sky %d pty %d",
                 env.pm10, env.pm25, env.temp, env.sky, env.pty);
    } else {
        ESP_LOGW(TAG, "env: fetch failed (services registered?)");
    }
}

static void fetch_arrivals(void) {
    pend_n = 0;
    int ok = fetch_gbis();
    ok |= fetch_tago();
    if (!ok) {
        fetch_fail++;
        ESP_LOGW(TAG, "both feeds failed #%d (keeping last data)", fetch_fail);
        return;
    }
    fetch_fail = 0;

    time_t now = kst_now();
    int applied = 0;
    for (int r = 0; r < N_ROWS; r++) {
        int old_has = rows[r].has_arrival;
        int old_eta = rows[r].arrtime;
        time_t old_fetch = rows[r].fetched;
        int old_veh = rows[r].veh;

        rows[r].has_arrival = 0;
        for (int i = 0; i < pend_n; i++) {
            if (ROWS[r].number == pend_no[i]) {
                rows[r].has_arrival = 1;
                rows[r].arrtime = pend_at[i];
                rows[r].arrtime2 = pend_at2[i];
                rows[r].seats = pend_seats[i];
                rows[r].veh = pend_veh[i];
                rows[r].fetched = now;
                applied++;
                break;
            }
        }

        // confirmed arrival (type 1): the close bus tracked last poll is gone.
        // Two detection paths, because neither feed alone goes silent at
        // every arrival (GBIS rolls its first-bus slot straight to the next
        // bus; TAGO drops vehicles at the stop but only covers some routes):
        //  - identity roll: the first-bus GBIS vehicle id changed — the old
        //    bus is out of the feed, so it passed (decisive, confirms at once)
        //  - feed silence: the merged feed stopped reporting the route;
        //    requires 2 consecutive silent polls so a one-poll feed blip
        //    (bus reappears, still approaching) is not counted as an arrival
        if (old_has && old_eta <= LOG_CONFIRM_SECS) {
            int roll = rows[r].has_arrival && old_veh >= 0 &&
                       rows[r].veh >= 0 && rows[r].veh != old_veh;
            // a close bus jumped far out (>8 min) and the new vehicle id is
            // unknown: it passed unseen during a feed gap (vehicle-roll can't
            // fire without the new id)
            int jumped = rows[r].has_arrival && old_veh >= 0 &&
                         rows[r].veh < 0 &&
                         rows[r].arrtime > old_eta + 480;
            if (roll || jumped) {
                // a vanish of a vehicle already confirmed within the last
                // 10 min is the feed's post-pass flicker, not another arrival
                int dup = old_veh >= 0 && old_veh == rows[r].veh_confirmed &&
                          now - rows[r].veh_conf_at < 600;
                if (!dup) {
                    log_arrival(r, old_fetch + old_eta, 1);
                    rows[r].veh_confirmed = old_veh;
                    rows[r].veh_conf_at = now;
                }
                rows[r].arrived_at = old_fetch + old_eta;
                // if the arrival happened during a feed gap, re-anchor the
                // display window to now so ARRIVING/JUST LEFT still show
                if (now - rows[r].arrived_at >
                    ARRIVING_SHOW_SECS + JUST_LEFT_SHOW_SECS)
                    rows[r].arrived_at = now;
                rows[r].confirm_pending = 0;
                rows[r].silent_polls = 0;
                rows[r].rel_pending = 0;   // normal confirm supersedes
            } else if (!rows[r].has_arrival) {
                if (!rows[r].confirm_pending) {
                    rows[r].pend_eta = old_eta;
                    rows[r].pend_fetch = old_fetch;
                    rows[r].confirm_pending = 1;
                    rows[r].silent_polls = 0;
                }
                if (++rows[r].silent_polls >= 2) {
                    int dup = old_veh >= 0 && old_veh == rows[r].veh_confirmed &&
                              now - rows[r].veh_conf_at < 600;
                    if (!dup) {
                        log_arrival(r, rows[r].pend_fetch + rows[r].pend_eta, 1);
                        rows[r].veh_confirmed = old_veh;
                        rows[r].veh_conf_at = now;
                    }
                    rows[r].arrived_at = rows[r].pend_fetch + rows[r].pend_eta;
                    if (now - rows[r].arrived_at >
                        ARRIVING_SHOW_SECS + JUST_LEFT_SHOW_SECS)
                        rows[r].arrived_at = now;
                    rows[r].confirm_pending = 0;
                    rows[r].rel_pending = 0;   // normal confirm supersedes
                }
            } else {
                rows[r].confirm_pending = 0;
                rows[r].silent_polls = 0;
            }
        } else if (rows[r].confirm_pending && !rows[r].has_arrival) {
            if (++rows[r].silent_polls >= 2) {
                log_arrival(r, rows[r].pend_fetch + rows[r].pend_eta, 1);
                rows[r].confirm_pending = 0;
            }
        } else {
            rows[r].confirm_pending = 0;
            rows[r].silent_polls = 0;
        }

        // outage arrival pending: resolved now that the feed is back.
        // If the route is still silent the bus arrived unseen — log it at
        // the due moment. If a bus is present again, cancel (it never came
        // or a new bus appeared; no double counting).
        if (rows[r].rel_pending) {
            if (!rows[r].has_arrival)
                log_arrival(r, rows[r].rel_due, 1);
            rows[r].rel_pending = 0;
        }

        // predicted arrival (type 0): first bus sighted within the capture
        // window. GBIS ETAs are ~60s-quantized (jumps like 137->14), so the
        // old 90s window was skipped by most buses; 240s matches the
        // confirmed window. Per-vehicle dedupe keeps one event per bus; the
        // 2-minute guard absorbs feed handoffs (GBIS vehicle id vs TAGO's
        // missing id) that would otherwise log the same bus twice.
        if (rows[r].has_arrival) {
            if (rows[r].arrtime <= LOG_PREDICT_SECS) {
                int dup = rows[r].logged && rows[r].veh_logged == rows[r].veh;
                int recent = rows[r].logged_ts &&
                             now + rows[r].arrtime - rows[r].logged_ts < 120;
                if (!dup && !recent) {
                    log_arrival(r, now + rows[r].arrtime, 0);
                    rows[r].logged = 1;
                    rows[r].veh_logged = rows[r].veh;
                    rows[r].logged_ts = now + rows[r].arrtime;
                }
            } else if (rows[r].arrtime > LOG_REARM_SECS) {
                rows[r].logged = 0;  // re-arm for the next bus
            }
        } else {
            rows[r].logged = 0;
        }
    }
    last_good = now;
    ESP_LOGI(TAG, "live: %d routes, %d pending entries", applied, pend_n);
}

// ---- arrival logging (NVS, one blob per day) ----
// event = 2 bytes: [type(1) | route_idx(3) | minute_of_day(11)]
// type 0 = predicted arrival (sighted within 4 min), type 1 = confirmed
// (tracked close bus left the feed: vehicle-id roll or 2-poll silence)
#define LOG_DAYS_KEEP  60
#define LOG_DAY_BYTES  2048

static nvs_handle_t log_nvs;
static int log_full;

static void log_init(void) {
    esp_err_t err = nvs_open("buslog", NVS_READWRITE, &log_nvs);
    if (err != ESP_OK) {
        log_nvs = 0;
        ESP_LOGW(TAG, "nvs open failed %s", esp_err_to_name(err));
        return;
    }
    // drop blobs older than LOG_DAYS_KEEP
    time_t cutoff_ts = kst_now() - LOG_DAYS_KEEP * 86400;
    struct tm ct;
    localtime_r(&cutoff_ts, &ct);
    char cutoff[16];
    snprintf(cutoff, sizeof(cutoff), "d%04d%02d%02d", ct.tm_year + 1900, ct.tm_mon + 1, ct.tm_mday);
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, "buslog", NVS_TYPE_BLOB, &it);
    while (res == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "d", 1) == 0 && strcmp(info.key, cutoff) < 0)
            nvs_erase_key(log_nvs, info.key);
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_commit(log_nvs);
    ESP_LOGI(TAG, "arrival log ready (keeps %d days)", LOG_DAYS_KEEP);
}

// event = (route_idx<<11) | minute_of_day, 2 bytes
static void log_arrival(int route_idx, time_t arrival_ts, int type) {
    if (!log_nvs) return;
    struct tm t;
    localtime_r(&arrival_ts, &t);
    char key[16];
    snprintf(key, sizeof(key), "d%04d%02d%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    uint8_t blob[LOG_DAY_BYTES];
    size_t len = sizeof(blob);
    if (nvs_get_blob(log_nvs, key, blob, &len) != ESP_OK) len = 0;
    if (len >= sizeof(blob) - 2) {
        log_full = 1;
        return;  // day full
    }
    int minute = t.tm_hour * 60 + t.tm_min;
    // bit 15 flags a public holiday (learner/analysis bucket these days
    // separately without needing an external calendar)
    uint16_t ev = (uint16_t)((type << 14) | (route_idx << 11) | (minute & 0x7FF));
    if (is_holiday(arrival_ts)) ev |= (1 << 15);
    blob[len++] = ev & 0xFF;
    blob[len++] = ev >> 8;
    if (nvs_set_blob(log_nvs, key, blob, len) != ESP_OK) {
        log_full = 1;
        return;
    }
    nvs_commit(log_nvs);
    ESP_LOGI(TAG, "logged arrival: route %d at %02d:%02d (%s)",
             ROWS[route_idx].number, minute / 60, minute % 60,
             type ? "confirmed" : "predicted");
}

// ---- wifi ----
static void wifi_init(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wc = {
        .sta = { .ssid = WIFI_SSID, .password = WIFI_PASS },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();
    int tries = 0;
    while (tries++ < 60) {
        esp_netif_ip_info_t ip;
        esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
        if (ip.ip.addr != 0) {
            ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ip.ip));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGW(TAG, "WiFi connect timeout");
}

// ---- UI ----
static void ui_build(void) {
    lv_obj_t* scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B0E14), 0);

    for (int i = 0; i < N_ROWS; i++) { rows[i].disp = -1; rows[i].anim = 0; rows[i].shown = -999; }  // snap on first live sighting

    hdr_label = lv_label_create(scr);
    lv_obj_set_style_text_color(hdr_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(hdr_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(hdr_label, "STOP: 07593");
    lv_obj_set_pos(hdr_label, 10, 6);

    // env line: colored spans — labels gray, values colored by AQ grade /
    // weather, separators spaced:  "PM10 12 | PM2.5 7 | 22C CLD"
    env_group = lv_spangroup_create(scr);
    lv_obj_set_style_text_font(env_group, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(env_group, lv_color_hex(0x96969A), LV_PART_MAIN);
    // vertically centered against the font-20 header line (22px at y=6)
    lv_obj_set_pos(env_group, 10, 8);
    lv_spangroup_set_align(env_group, LV_TEXT_ALIGN_LEFT);
    for (int i = 0; i < 7; i++) {
        env_span[i] = lv_spangroup_new_span(env_group);
        lv_style_set_text_color(&env_span[i]->style, lv_color_hex(0x96969A));
    }
    lv_obj_add_flag(env_group, LV_OBJ_FLAG_HIDDEN);

    // clock: right-aligned to a fixed box ending at x=310, so the right
    // margin stays 10px (mirroring the left) regardless of the time shown
    clock_label = lv_label_create(scr);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(0xFF5252), 0);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(clock_label, 244, 6);
    lv_obj_set_width(clock_label, 66);
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(clock_label, "");

    for (int i = 0; i < N_ROWS; i++) {
        // rows start just below the header (46 + 10px), clearing the bottom
        int y = 56 + i * 60;

        route_labels[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(route_labels[i], lv_color_hex(0xFFD400), 0);
        lv_obj_set_style_text_font(route_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_set_pos(route_labels[i], 10, y);

        time1_labels[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(time1_labels[i], lv_color_white(), 0);
        lv_obj_set_style_text_font(time1_labels[i], &lv_font_mono28, 0);
        lv_obj_set_pos(time1_labels[i], 160, y);

        time2_labels[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(time2_labels[i], lv_color_hex(0x96969A), 0);
        lv_obj_set_style_text_font(time2_labels[i], &lv_font_montserrat_16, 0);
        lv_obj_set_pos(time2_labels[i], 160, y + 30);

        char no[8];
        snprintf(no, sizeof(no), "%d", ROWS[i].number);
        lv_label_set_text(route_labels[i], no);

        // remaining seats, e.g. "4103 (35)" — small gray bracket after the
        // route number so the subtext line stays free for the next-bus time
        seats_labels[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(seats_labels[i], lv_color_hex(0x96969A), 0);
        lv_obj_set_style_text_font(seats_labels[i], &lv_font_montserrat_16, 0);
        lv_obj_set_pos(seats_labels[i], 10 + lv_obj_get_width(route_labels[i]) + 4, y + 8);
        lv_label_set_text(seats_labels[i], "");
    }
}

static int now_hm(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_hour * 60 + t.tm_min;
}

static int weekday_p(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_wday >= 1 && t.tm_wday <= 5);
}

// ---- public holidays (한국천문연구원 특일정보, data.go.kr service B090041) ----
// Fetched via getRestDeInfo (공휴일: includes 대체공휴일 and 임시공휴일
// published late), current+next month, cached in NVS.
#define HOLIDAY_REFRESH_SECS 86400
#define HOLIDAY_NVS_NS       "holiday"
#define HOLIDAY_NVS_KEY      "days"
static int holi_days[64];
static int holi_n;
static time_t holi_next;

static void holiday_save(void) {
    nvs_handle_t h;
    if (nvs_open(HOLIDAY_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, HOLIDAY_NVS_KEY, holi_days, holi_n * sizeof(int));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void holiday_load(void) {
    nvs_handle_t h;
    if (nvs_open(HOLIDAY_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(holi_days);
        if (nvs_get_blob(h, HOLIDAY_NVS_KEY, holi_days, &len) == ESP_OK)
            holi_n = len / sizeof(int);
        nvs_close(h);
    }
}

static void fetch_holidays(void) {
    holi_n = 0;
    time_t now = kst_now();
    for (int m = 0; m < 2; m++) {  // current + next month
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_mon += m;
        tm.tm_mday = 1;
        mktime(&tm);
        char url[512];
        snprintf(url, sizeof(url),
                 "https://apis.data.go.kr/B090041/openapi/service/SpcdeInfoService/"
                 "getRestDeInfo?serviceKey=%s&solYear=%04d&solMonth=%02d"
                 "&numOfRows=50&pageNo=1&_type=json",
                 API_KEY, tm.tm_year + 1900, tm.tm_mon + 1);
        if (!http_get(url)) continue;
        const char* p = g_body;
        while (p && (p = strchr(p, '{')) && holi_n < 64) {
            const char* e = strchr(p + 1, '}');
            if (!e) break;
            char ish[4];
            if (item_str(p, e, "isHoliday", ish, sizeof(ish)) && strcmp(ish, "Y") == 0) {
                long ld = item_num(p, e, "locdate");
                if (ld >= 20250101) {
                    int dup = 0;
                    for (int i = 0; i < holi_n; i++)
                        if (holi_days[i] == ld) dup = 1;
                    if (!dup) holi_days[holi_n++] = (int)ld;
                }
            }
            p = e + 1;
        }
    }
    if (holi_n) {
        qsort(holi_days, holi_n, sizeof(int), cmp_int);
        holiday_save();
        ESP_LOGI(TAG, "holidays: %d dates loaded (this+next month)", holi_n);
    } else {
        ESP_LOGW(TAG, "holidays: fetch failed (service B090041 registered?)");
    }
}

static int is_holiday(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    int d = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    for (int i = 0; i < holi_n; i++)
        if (holi_days[i] == d) return 1;
    return 0;
}

// "weekend" = Saturday, Sunday, or a public holiday (incl. 대체/임시)
static int weekend_p(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_wday == 0 || t.tm_wday == 6) return 1;
    return is_holiday(now);
}

// ---- learned schedule: rebuild rings from the buslog at boot ----
static int learner_daytype_now(void) {
    return weekend_p() ? LEARNER_DT_WEEKEND : LEARNER_DT_WEEKDAY;
}

// learn the confirmed arrivals of one buslog day blob.
// Buffers are PSRAM: the boot task stack is only ~8KB and a day's arrival
// matrix (7 routes x 96) does not fit on it.
static void learner_absorb_day(const char* key, const uint8_t* blob, size_t len,
                               int skip_today) {
    time_t now = kst_now();
    struct tm tmt;
    localtime_r(&now, &tmt);
    char today[16];
    snprintf(today, sizeof(today), "d%04d%02d%02d",
             tmt.tm_year + 1900, tmt.tm_mon + 1, tmt.tm_mday);
    if (skip_today && strcmp(key, today) == 0) return;

    int hol = 0;
    for (int r = 0; r < N_ROWS; r++) learn_confn[r] = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        uint16_t ev = blob[i] | (blob[i + 1] << 8);
        if (ev & 0x8000) hol = 1;
        if (ev & 0x4000) {  /* confirmed arrival */
            int r = (ev >> 11) & 0x7;
            int mm = ev & 0x7FF;
            if (r < N_ROWS && learn_confn[r] < LEARNER_MAX_ARR)
                learn_conf[r * LEARNER_MAX_ARR + learn_confn[r]++] = mm;
        }
    }
    int y, m, d;
    sscanf(key, "d%04d%02d%02d", &y, &m, &d);
    int dt = LEARNER_DT_WEEKEND;
    if (!hol) {
        struct tm dtm = {0};
        dtm.tm_year = y - 1900;
        dtm.tm_mon = m - 1;
        dtm.tm_mday = d;
        mktime(&dtm);
        if (dtm.tm_wday >= 1 && dtm.tm_wday <= 5) dt = LEARNER_DT_WEEKDAY;
    }
    for (int r = 0; r < N_ROWS; r++)
        if (learn_confn[r] > 0)
            learner_learn_day(&learners[r], dt, &learn_conf[r * LEARNER_MAX_ARR],
                              learn_confn[r]);
}

static void learner_rebuild(void) {
    for (int i = 0; i < N_ROWS; i++) learner_init(&learners[i], ROWS[i].number);
    learner_ready = 0;

    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find(NVS_DEFAULT_PART_NAME, "buslog", NVS_TYPE_BLOB, &it);
    while (res == ESP_OK && it) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, "d", 1) == 0) {
            static uint8_t blob[LOG_DAY_BYTES];
            size_t len = sizeof(blob);
            if (nvs_get_blob(log_nvs, info.key, blob, &len) == ESP_OK && len >= 2)
                learner_absorb_day(info.key, blob, len, 1);
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    learner_ready = 1;
    time_t now = kst_now();
    struct tm tmt;
    localtime_r(&now, &tmt);
    learner_last_day = (tmt.tm_year + 1900) * 10000 + (tmt.tm_mon + 1) * 100 + tmt.tm_mday;
    ESP_LOGI(TAG, "learner: rings rebuilt from buslog");
}

// at the night transition, the current day is complete: learn it
static void learner_learn_today(void) {
    if (!learner_ready) return;
    time_t now = kst_now();
    struct tm tmt;
    localtime_r(&now, &tmt);
    int day = (tmt.tm_year + 1900) * 10000 + (tmt.tm_mon + 1) * 100 + tmt.tm_mday;
    if (day == learner_last_day) return;

    char key[16];
    snprintf(key, sizeof(key), "d%04d%02d%02d", tmt.tm_year + 1900, tmt.tm_mon + 1, tmt.tm_mday);
    static uint8_t blob[LOG_DAY_BYTES];
    size_t len = sizeof(blob);
    if (nvs_get_blob(log_nvs, key, blob, &len) == ESP_OK && len >= 2)
        learner_absorb_day(key, blob, len, 0);
    learner_last_day = day;
    ESP_LOGI(TAG, "learner: learned day %d", day);
}

// learned next arrivals after now (minutes of day). Returns 1 if available.
static int learner_next_now(int r, int* next1, int* next2) {
    if (!learner_ready) return 0;
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    int hm = t.tm_hour * 60 + t.tm_min;
    return learner_next(&learners[r], learner_daytype_now(), hm, next1, next2);
}

static double learner_conf_now(int r) {
    if (!learner_ready) return 0;
    return learner_confidence(&learners[r], learner_daytype_now());
}

// seats only make sense on the express routes (직행좌석/광역)
static int show_seats(int route_no) {
    return route_no == 4103 || route_no == 9409 || route_no == 9507;
}

// any bus within CLOSE_WIN_SECS of the stop → burst-cadence so the
// ARRIVING state is confirmed (or corrected) within ~30s
static int any_close_bus(void) {
    time_t now = kst_now();
    for (int i = 0; i < N_ROWS; i++) {
        if (rows[i].has_arrival &&
            rows[i].arrtime - (int)(now - rows[i].fetched) <= CLOSE_WIN_SECS)
            return 1;
    }
    return 0;
}

// weekday rush window (also drives the commute-headway selection)
static int is_peak_now(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    if (weekend_p()) return 0;
    return (t.tm_hour >= CAD_RUSH_START && t.tm_hour < CAD_RUSH_END);
}

// every configured route past its last bus → nothing left to query
// until tomorrow morning (event-based night mode; saves quota on late
// evenings beyond the time-based window)
static int all_done_now(void) {
    int hm = now_hm();
    for (int i = 0; i < N_ROWS; i++) {
        if (!ROWS[i].has_sched) return 0;  // can't know → keep polling
        if (hm <= ROWS[i].last_hm + ROWS[i].to_stop) return 0;
    }
    return 1;
}

// earliest first-bus arrival at the stop (minutes since midnight), -1 unknown
static int first_arrival_min(void) {
    int best = -1;
    for (int i = 0; i < N_ROWS; i++) {
        if (!ROWS[i].has_sched) continue;
        int at = ROWS[i].first_hm + ROWS[i].to_stop;
        if (best < 0 || at < best) best = at;
    }
    return best;
}

// current polling cadence in seconds, or -1 outside the polling window
// (night: schedule-only display, no API calls, lights out). Wakes ~30 min
// before the earliest first bus (later: the learner's real first arrival).
static int cadence_now(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    int h = t.tm_hour;
    int first = first_arrival_min();
    int wake_min = (first >= 0) ? first - WAKE_MINS : CAD_START_HOUR * 60;
    if (h * 60 + t.tm_min < wake_min || h >= CAD_NIGHT_END)
        return -1;
    if (all_done_now())
        return -1;
    if (any_close_bus())
        return CLOSE_REFRESH;
    if (!weekend_p() && h >= CAD_RUSH_START && h < CAD_RUSH_END)
        return CAD_RUSH_SECS;
    if (h >= CAD_EVE_START)
        return CAD_EVE_SECS;
    if (h < CAD_RUSH_START)
        return CAD_MORN_SECS;
    return CAD_DAY_SECS;
}

// air quality grade colors (Korea standard): 좋음 blue, 보통 green,
// 나쁨 orange, 매우나쁨 red
static lv_color_t pm10_color(int v) {
    if (v <= 0) return lv_color_hex(0x96969A);
    if (v <= 30) return lv_color_hex(0x3399FF);
    if (v <= 80) return lv_color_hex(0x4CAF50);
    if (v <= 150) return lv_color_hex(0xFF7A00);
    return lv_color_hex(0xFF5252);
}

static lv_color_t pm25_color(int v) {
    if (v <= 0) return lv_color_hex(0x96969A);
    if (v <= 15) return lv_color_hex(0x3399FF);
    if (v <= 35) return lv_color_hex(0x4CAF50);
    if (v <= 75) return lv_color_hex(0xFF7A00);
    return lv_color_hex(0xFF5252);
}

static lv_color_t wx_color(int cond) {
    if (cond == 0) return lv_color_hex(0xFFD400);      // SUN
    if (cond == 1) return lv_color_hex(0xC0C4CC);      // CLD
    if (cond == 2) return lv_color_hex(0xC0C4CC);      // OVC
    if (cond == 3) return lv_color_hex(0x66B2FF);      // RAIN
    if (cond == 4) return lv_color_hex(0xE6E9EF);      // SNOW
    return lv_color_hex(0x96969A);                     // SHWR / unknown
}

static void set_env_span(int i, const char* txt, lv_color_t col) {
    lv_span_set_text(env_span[i], txt);
    lv_style_set_text_color(&env_span[i]->style, col);
}

// subtext line: seats bracket + second-bus "+Xm" — also shown during
// ARRIVING/JUST LEFT so the next bus stays visible while the row
// acknowledges the one that just left
static void set_subtext(int i, time_t now) {
    char sub[32] = "";
    if (rows[i].seats >= 0 && show_seats(ROWS[i].number) &&
        now - rows[i].fetched < 240) {
        snprintf(sub, sizeof(sub), "(%d)", rows[i].seats);
        // stick the bracket to the route number: LVGL-aligns right of it,
        // vertically centered (no manual width math)
        lv_obj_align_to(seats_labels[i], route_labels[i], LV_ALIGN_OUT_RIGHT_MID, 4, 0);
    }
    lv_label_set_text(seats_labels[i], sub);
    sub[0] = 0;
    // next-bus estimate: the feed's current second bus wins (live trumps,
    // gray); the learned next2 fills only when the feed reports none (blue)
    int l1, l2;
    int hm = now_hm();
    lv_color_t sub_col = lv_color_hex(0x96969A);
    if (rows[i].arrtime2 >= 0 && now - rows[i].fetched < 240) {
        int rem2 = rows[i].arrtime2 - (int)(now - rows[i].fetched);
        if (rem2 < 0) rem2 = 0;
        snprintf(sub, sizeof(sub), "+%dm", (rem2 + 59) / 60);
    } else if (learner_conf_now(i) >= LEARNED_SUBTEXT_CONF &&
               learner_next_now(i, &l1, &l2) && l2 > hm) {
        snprintf(sub, sizeof(sub), "+%dm", l2 - hm);
        sub_col = lv_color_hex(0x3399FF);
    }
    lv_obj_set_style_text_color(time2_labels[i], sub_col, 0);
    lv_label_set_text(time2_labels[i], sub);
}

// rough sunrise/sunset (KST, lat ~37.5N) — accurate to ~±15 min, plenty for
// labeling SUN vs CLEAR on a clear day; no API or config needed
#define SOLAR_NOON_MIN (12 * 60 + 20)
static void solar_times(int doy, int* rise_min, int* set_min) {
    double decl = 23.45 * sin(2.0 * 3.141592653589793 * (284 + doy) / 365.0);
    double lat = 37.4 * 3.141592653589793 / 180.0;
    double cosha = -tan(lat) * tan(decl * 3.141592653589793 / 180.0);
    if (cosha < -1.0) cosha = -1.0;
    if (cosha > 1.0) cosha = 1.0;
    int half = (int)(acos(cosha) * 180.0 / 3.141592653589793 * 4.0);
    *rise_min = SOLAR_NOON_MIN - half;
    *set_min = SOLAR_NOON_MIN + half;
}

static int is_daytime(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    int rise, set;
    solar_times(t.tm_yday + 1, &rise, &set);
    int hm = t.tm_hour * 60 + t.tm_min;
    return hm > rise && hm < set;
}

static void ui_update(void) {
    char buf[96];
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);

    snprintf(buf, sizeof(buf), "STOP: 07593 (%ds)", refresh_secs);
    if (!log_nvs) {
        strcat(buf, " NOLOG");
    } else if (log_full) {
        strcat(buf, " LOG!");
    }
    int status_issue = fetch_fail || !log_nvs || log_full;
    // header rotation: weather/PM 10s, status 5s, learning status 5s
    int slot = t.tm_sec % 20;
    int env_phase = slot < 10;
    int learn_phase = learner_ready && slot >= 15 && !status_issue;
    if ((env.have_pm || env.have_wx) && env_phase && !status_issue) {
        lv_obj_clear_flag(env_group, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(hdr_label, "");
        char v[8];
        int wcond = 0;
        if (env.pty >= 1 && env.pty <= 3) wcond = env.pty == 3 ? 4 : 3;
        else if (env.pty == 4) wcond = 5;
        else if (env.sky == 1) wcond = 0;
        else if (env.sky == 3) wcond = 1;
        else if (env.sky == 4) wcond = 2;

        set_env_span(0, "PM10 ", lv_color_white());
        snprintf(v, sizeof(v), "%d", env.pm10);
        if (env.pm10 <= 0) strcpy(v, "-");   // 0 = no measurement overnight
        set_env_span(1, v, pm10_color(env.pm10));
        set_env_span(2, " | PM2.5 ", lv_color_white());
        snprintf(v, sizeof(v), "%d", env.pm25);
        if (env.pm25 <= 0) strcpy(v, "-");
        set_env_span(3, v, pm25_color(env.pm25));
        if (env.have_wx && env.temp >= 0) {
            set_env_span(4, " | ", lv_color_hex(0x96969A));
            snprintf(v, sizeof(v), "%dC ", env.temp);
            set_env_span(5, v, lv_color_hex(0x9BD8FF));
            const char* wname = "SKY";
            lv_color_t wcol = wx_color(wcond);
            if (wcond == 0 && !is_daytime()) {
                wname = "CLR";
                wcol = lv_color_hex(0x7A9BB8);   // dim night blue, not sunny yellow
            } else if (wcond == 0) {
                wname = "SUN";
            } else if (wcond == 1) wname = "CLD";
            else if (wcond == 2) wname = "OVC";
            else if (wcond == 3) wname = "RAIN";
            else if (wcond == 4) wname = "SNOW";
            else if (wcond == 5) wname = "SHWR";
            set_env_span(6, wname, wcol);
        } else {
            set_env_span(4, "", lv_color_hex(0x96969A));
            set_env_span(5, "", lv_color_hex(0x96969A));
            set_env_span(6, "", lv_color_hex(0x96969A));
        }
        lv_obj_invalidate(env_group);
    } else if (learn_phase) {
        lv_obj_add_flag(env_group, LV_OBJ_FLAG_HIDDEN);
        // average learned confidence across routes (current day-type)
        double tot = 0;
        int n = 0;
        for (int i = 0; i < N_ROWS; i++) {
            double c = learner_conf_now(i);
            if (c > 0) { tot += c; n++; }
        }
        if (n > 0) {
            snprintf(buf, sizeof(buf), "LEARN %d%% (%d/%d)",
                     (int)(100 * tot / n + 0.5), n, N_ROWS);
            lv_label_set_text(hdr_label, buf);
            lv_obj_set_style_text_color(hdr_label, lv_color_hex(0x9BD8FF), 0);
        } else {
            lv_label_set_text(hdr_label, buf);
            lv_obj_set_style_text_color(hdr_label, lv_color_white(), 0);
        }
    } else {
        lv_obj_add_flag(env_group, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(hdr_label, buf);
        lv_color_t hdr_col = lv_color_white();
        if (fetch_fail) hdr_col = lv_color_hex(0xFF7A00);
        else if (!log_nvs || log_full) hdr_col = lv_color_hex(0xFFD400);
        lv_obj_set_style_text_color(hdr_label, hdr_col, 0);
    }

    snprintf(buf, sizeof(buf), "%d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(clock_label, buf);

    int hm = now_hm();
    int we = weekend_p();
    for (int i = 0; i < N_ROWS; i++) {
        const row_cfg_t* rc = &ROWS[i];
        row_state_t* rs = &rows[i];
        char b[32];

        // zombie countdown: the feed went quiet on a bus that hasn't reached
        // its expected arrival moment yet — keep counting the last known ETA
        // instead of dropping to "--"
        if (!rs->has_arrival && !rs->zombie && rs->last_eta > 0 &&
            now < rs->fetched + rs->last_eta)
            rs->zombie = 1;
        if (rs->has_arrival) rs->zombie = 0;

        if (rs->has_arrival || rs->zombie) {
            // the bus we were watching just arrived; the next bus is live,
            // but still show ARRIVING/JUST LEFT briefly before switching
            if (rs->has_arrival && rs->arrived_at &&
                now < rs->arrived_at + ARRIVING_SHOW_SECS) {
                lv_label_set_text(time1_labels[i], "ARRIVING");
                lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x4CAF50), 0);
                set_subtext(i, now);
                rs->anim = 0;
                continue;
            }
            if (rs->has_arrival && rs->arrived_at &&
                now < rs->arrived_at + ARRIVING_SHOW_SECS + JUST_LEFT_SHOW_SECS) {
                lv_label_set_text(time1_labels[i], "JUST LEFT");
                lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0xA03030), 0);
                set_subtext(i, now);
                rs->anim = 0;
                continue;
            }
            if (rs->arrived_at) {
                // acknowledgment window over: the next bus is a discrete
                // event — snap to it rather than racing up
                rs->arrived_at = 0;
                rs->disp = -1;
            }
            int rem;
            if (rs->has_arrival) {
                rem = rs->arrtime - (int)(now - rs->fetched);
                if (rem < 0) rem = 0;
                rs->last_eta = rem;
            } else {
                rem = rs->last_eta - (int)(now - rs->fetched);
                if (rem < 0) rem = 0;
                // countdown exhausted: ARRIVING briefly, then release
                if (rem == 0 &&
                    now >= rs->fetched + rs->last_eta + ARRIVING_SHOW_SECS) {
                    rs->arrived_at = rs->fetched + rs->last_eta;
                    rs->rel_pending = 1;   // log once the feed returns
                    rs->rel_due = rs->arrived_at;
                    rs->zombie = 0;
                    rs->last_eta = 0;
                }
            }
            uint32_t row_age = (uint32_t)(now - rs->fetched);
            // exhausted countdown with a dead feed: acknowledge the arrival
            // (ARRIVING → JUST LEFT) like the zombie path, then release to
            // the schedule instead of flashing "--"
            if (rem == 0 && row_age > 120 && !rs->zombie) {
                time_t anchor = rs->arrived_at;
                if (!anchor) {
                    if (!rs->due_at) rs->due_at = now;
                    anchor = rs->due_at;
                }
                int since = (int)(now - anchor);
                if (since < ARRIVING_SHOW_SECS) {
                    lv_label_set_text(time1_labels[i], "ARRIVING");
                    lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x4CAF50), 0);
                } else if (since < ARRIVING_SHOW_SECS + JUST_LEFT_SHOW_SECS) {
                    lv_label_set_text(time1_labels[i], "JUST LEFT");
                    lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0xA03030), 0);
                } else {
                    rs->has_arrival = 0;   // release to the schedule/--
                    rs->due_at = 0;
                    rs->rel_pending = 1;   // log once the feed returns
                    rs->rel_due = anchor;
                    lv_label_set_text(time1_labels[i], "--");
                    lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x505860), 0);
                }
                rs->anim = 0;
            } else {
                // the 100ms anim timer races disp toward this target
                rs->target = rem;
                rs->anim = 1;
                if (rs->disp < 0) rs->disp = rem;
                if (rs->disp == rem) {
                    fmt_secs(rem, b, sizeof(b));
                    lv_label_set_text(time1_labels[i], b);
                }
                lv_color_t col = rem == 0 ? lv_color_hex(0x4CAF50)
                              : (row_age < 60 ? lv_color_white()
                              : (row_age < 180 ? lv_color_hex(0xFFD400) : lv_color_hex(0xFF7A00)));
                lv_obj_set_style_text_color(time1_labels[i], col, 0);
            }

            set_subtext(i, now);
            continue;
        }

        // no live arrival: schedule placeholder. A just-confirmed arrival
        // (the watched bus left the feed) keeps ARRIVING on screen briefly
        // instead of jumping straight to the next-bus estimate, then
        // JUST LEFT (dark red) before the row releases
        if (rs->arrived_at && now < rs->arrived_at + ARRIVING_SHOW_SECS) {
            lv_label_set_text(time1_labels[i], "ARRIVING");
            lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x4CAF50), 0);
            set_subtext(i, now);
            rs->anim = 0;
            continue;
        }
        if (rs->arrived_at &&
            now < rs->arrived_at + ARRIVING_SHOW_SECS + JUST_LEFT_SHOW_SECS) {
            lv_label_set_text(time1_labels[i], "JUST LEFT");
            lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0xA03030), 0);
            set_subtext(i, now);
            rs->anim = 0;
            continue;
        }
        if (rs->arrived_at) {
            rs->arrived_at = 0;   // window over: next live sighting snaps
            rs->disp = -1;
        }
        // learned fill: a trustworthy learned next arrival replaces the
        // static estimate / "--" with a blue countdown (live still trumps)
        if (learner_conf_now(i) >= LEARNED_FILL_CONF) {
            int l1, l2;
            if (learner_next_now(i, &l1, &l2)) {
                int rem_s = l1 * 60 - (hm * 60 + t.tm_sec);
                // learned fills respect the live-reporting window: inside
                // ~15 min, live silence means unknown — show "--" instead
                if (rem_s > LEARNED_FILL_MIN_SECS && rem_s < 90 * 60) {
                    rs->target = rem_s;
                    rs->anim = 1;
                    if (rs->disp < 0) rs->disp = rem_s;
                    if (rs->disp == rem_s) {
                        fmt_secs(rem_s, b, sizeof(b));
                        lv_label_set_text(time1_labels[i], b);
                    }
                    lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x3399FF), 0);
                    set_subtext(i, now);
                    continue;
                }
            }
            rs->anim = 0;
        }
        if (rc->has_sched) {
            int hw = we ? rc->hw_weekend : (is_peak_now() ? rc->hw_commute : rc->hw_weekday);
            int first_at_stop = rc->first_hm + rc->to_stop;
            int last_at_stop = rc->last_hm + rc->to_stop;
            if (hm < first_at_stop) {
                snprintf(b, sizeof(b), "(%02d:%02d)", first_at_stop / 60, first_at_stop % 60);
                lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x3399FF), 0);
            } else if (hm > last_at_stop) {
                // last bus has passed our stop: no more arrivals today
                strcpy(b, "DONE");
                lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x505860), 0);
            } else {
                // count down to the next headway-based departure
                int cycle = hw * 60;
                int since_first = (hm * 60 + t.tm_sec) - first_at_stop * 60;
                int rem = (cycle - (since_first % cycle)) % cycle;
                if (rem < 900) {
                    // inside the live-reporting window: if a bus were really
                    // this close, live data would confirm it — so stay silent
                    strcpy(b, "--");
                    lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x505860), 0);
                } else {
                    snprintf(b, sizeof(b), "~%dm", (rem + 59) / 60);
                    lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x3399FF), 0);
                }
            }
            lv_label_set_text(time1_labels[i], b);
            set_subtext(i, now);
            rs->anim = 0;
            // short schedule blips keep disp so the live handoff animates;
            // long gaps (>5 min, e.g. overnight) snap to avoid a stale chase
            if (now - rs->fetched > 300) rs->disp = -1;
        } else {
            lv_label_set_text(time1_labels[i], "--");
            lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x505860), 0);
            set_subtext(i, now);
            rs->anim = 0;
            if (now - rs->fetched > 300) rs->disp = -1;
        }
    }
}

static void ui_tick_cb(lv_timer_t* t) { ui_update(); }

// 33ms chase timer (~30fps): when a row's target changes (feed update, next
// bus, regression), race the displayed seconds toward it with a smooth
// velocity ramp — accelerate to cruise, ease back down into the target —
// so big gaps blur through the numbers and small ones settle invisibly.
static void ui_anim_cb(lv_timer_t* t) {
    for (int i = 0; i < N_ROWS; i++) {
        row_state_t* rs = &rows[i];
        if (!rs->anim || rs->disp == rs->target)
            continue;
        int gap = rs->target - rs->disp;
        if (gap >= -2 && gap <= 2) {
            // tiny gap (steady countdown drift, small corrections): settle
            rs->disp = rs->target;
            rs->vel = 0;
        } else {
            // desired velocity: ~1s/tick near the target, up to 8s/tick far
            int desired = (abs(gap) / 8 + 1) * (gap > 0 ? 1 : -1);
            if (desired > 8) desired = 8;
            else if (desired < -8) desired = -8;
            int dv = (desired - rs->vel) / 4;      // C truncates toward zero
            if (dv == 0 && desired != rs->vel)      // ...so guard against stall
                dv = (desired > rs->vel) ? 1 : -1;
            rs->vel += dv;
            rs->disp += rs->vel;
            if ((gap > 0 && rs->disp > rs->target) || (gap < 0 && rs->disp < rs->target))
                rs->disp = rs->target;
        }
        if (rs->disp != rs->shown) {
            rs->shown = rs->disp;
            char b[32];
            if (rs->disp == rs->target) {
                fmt_secs(rs->target, b, sizeof(b));
                if (rs->target == 0)
                    lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x4CAF50), 0);
            } else {
                snprintf(b, sizeof(b), "%02dm %02ds", rs->disp / 60, rs->disp % 60);
            }
            lv_label_set_text(time1_labels[i], b);
        }
    }
}

static void wifi_ensure(void) {
    esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0)
        return;  // connected
    ESP_LOGW(TAG, "wifi down, reconnecting");
    esp_wifi_start();
    esp_wifi_connect();
    for (int i = 0; i < 30; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr != 0) {
            ESP_LOGI(TAG, "wifi reconnected");
            esp_sntp_stop();
            esp_sntp_init();
            return;
        }
    }
    ESP_LOGW(TAG, "wifi reconnect timeout");
}

// ---- refresh task ----
static void refresh_task(void* arg) {
    (void)arg;
    int wifi_tick = 0;
    for (;;) {
        time_t now = kst_now();
        if (now < 1600000000) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (++wifi_tick % 30 == 0)
            wifi_ensure();
        if (now - last_refresh >= refresh_secs) {
            int cad = cadence_now();
            if (cad < 0) {
                // night: schedule-only display, no API calls, lights out
                for (int i = 0; i < N_ROWS; i++) rows[i].has_arrival = 0;
                learner_learn_today();   // the day is complete: learn it
                if (backlight_on) {
                    bsp_display_brightness_set(0);
                    backlight_on = 0;
                }
                refresh_secs = CAD_OFF_SECS;
            } else {
                if (!backlight_on) {
                    bsp_display_brightness_set(100);
                    backlight_on = 1;
                }
                fetch_arrivals();
                if (fetch_fail) {
                    refresh_secs = RETRY_REFRESH;
                } else {
                    refresh_secs = cadence_now();
                }
            }
            last_refresh = now;
        } else if (now - env_next >= ENV_REFRESH_SECS) {
            fetch_env();
            env_next = now;
        } else if (now - holi_next >= HOLIDAY_REFRESH_SECS) {
            fetch_holidays();
            holi_next = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "bus board starting");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
        .rotate = LV_DISP_ROT_NONE,
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_brightness_set(100);

    wifi_init();
    log_init();
    learners = heap_caps_malloc(N_ROWS * sizeof(learner_t), MALLOC_CAP_SPIRAM);
    learn_conf = heap_caps_malloc(N_ROWS * LEARNER_MAX_ARR * sizeof(int), MALLOC_CAP_SPIRAM);
    learn_confn = heap_caps_malloc(N_ROWS * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!learners || !learn_conf || !learn_confn) {
        ESP_LOGE(TAG, "learner: PSRAM alloc failed");
    }
    learner_rebuild();                          // rings from the buslog

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();
    int ntp_tries = 0;
    while (kst_now() < 1600000000 && ntp_tries++ < 30) vTaskDelay(pdMS_TO_TICKS(500));
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    if (getaddrinfo("apis.data.go.kr", NULL, &hints, &res) == 0 && res) {
        ESP_LOGI(TAG, "dns ok: apis.data.go.kr -> %s",
                 inet_ntoa(((struct sockaddr_in*)res->ai_addr)->sin_addr));
        freeaddrinfo(res);
    } else {
        ESP_LOGW(TAG, "dns FAILED for apis.data.go.kr");
    }
    ESP_LOGI(TAG, "heap free: internal %d, psram %d",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    env_next = kst_now() - ENV_REFRESH_SECS;  // first env fetch on the first loop pass
    holiday_load();                            // cached holidays from NVS
    holi_next = kst_now() - HOLIDAY_REFRESH_SECS;  // refresh on the first loop pass

    bsp_display_lock(0);
    ui_build();
    ui_update();
    lv_timer_create(ui_tick_cb, 1000, NULL);
    lv_timer_create(ui_anim_cb, 33, NULL);
    bsp_display_unlock();

    last_refresh = kst_now() - refresh_secs;

    xTaskCreatePinnedToCore(refresh_task, "refresh", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "bus board running");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
