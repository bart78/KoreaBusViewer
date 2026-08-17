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
#include "nvs.h"
#include "../lv_font_mono28.c"

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

#define START_HOUR    4
#define END_HOUR      22
#define PEAK_REFRESH  15
#define OFF_REFRESH   60
#define RETRY_REFRESH 20

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
    int logged;       // arrival already logged to NVS
    time_t logged_ts; // arrival moment of the last predicted log
    time_t fetched;
    int disp;         // smoothed seconds shown on screen (-1 = snap to live)
    int target;       // seconds the display should chase toward
    int anim;         // 1 = the 33ms anim timer owns this row's text
    int zombie;       // feed quiet but bus not yet due: keep counting
    int last_eta;     // last live ETA before the row left the live state
    time_t arrived_at; // confirmed arrival moment (shows ARRIVING briefly)
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
            if (roll) {
                log_arrival(r, old_fetch + old_eta, 1);
                rows[r].arrived_at = old_fetch + old_eta;
                rows[r].confirm_pending = 0;
                rows[r].silent_polls = 0;
            } else if (!rows[r].has_arrival) {
                if (!rows[r].confirm_pending) {
                    rows[r].pend_eta = old_eta;
                    rows[r].pend_fetch = old_fetch;
                    rows[r].confirm_pending = 1;
                    rows[r].silent_polls = 0;
                }
                if (++rows[r].silent_polls >= 2) {
                    log_arrival(r, rows[r].pend_fetch + rows[r].pend_eta, 1);
                    rows[r].arrived_at = rows[r].pend_fetch + rows[r].pend_eta;
                    rows[r].confirm_pending = 0;
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

    for (int i = 0; i < N_ROWS; i++) { rows[i].disp = -1; rows[i].anim = 0; }  // snap on first live sighting

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
    lv_obj_set_pos(env_group, 10, 9);
    lv_spangroup_set_align(env_group, LV_TEXT_ALIGN_LEFT);
    for (int i = 0; i < 7; i++) {
        env_span[i] = lv_spangroup_new_span(env_group);
        lv_style_set_text_color(&env_span[i]->style, lv_color_hex(0x96969A));
    }
    lv_obj_add_flag(env_group, LV_OBJ_FLAG_HIDDEN);

    clock_label = lv_label_create(scr);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(0xFF5252), 0);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(clock_label, 260, 6);

    for (int i = 0; i < N_ROWS; i++) {
        int y = 46 + i * 60;

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
        lv_obj_set_pos(time2_labels[i], 160, y + 36);

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

// seats only make sense on the express routes (직행좌석/광역)
static int show_seats(int route_no) {
    return route_no == 4103 || route_no == 9409 || route_no == 9507;
}

static int is_peak_now(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    if (weekend_p()) return 0;
    return (t.tm_hour >= 6 && t.tm_hour < 9) || (t.tm_hour >= 15 && t.tm_hour < 19);
}

// any bus within a minute of the stop → poll at peak cadence so the
// ARRIVING state is confirmed (or corrected) within ~15s instead of 60s.
// The 60s trigger bounds the extra quota cost (~4 calls per arrival).
static int any_close_bus(void) {
    time_t now = kst_now();
    for (int i = 0; i < N_ROWS; i++) {
        if (rows[i].has_arrival &&
            rows[i].arrtime - (int)(now - rows[i].fetched) <= 60)
            return 1;
    }
    return 0;
}

// air quality grade colors (Korea standard): 좋음 blue, 보통 green,
// 나쁨 orange, 매우나쁨 red
static lv_color_t pm10_color(int v) {
    if (v < 0) return lv_color_hex(0x96969A);
    if (v <= 30) return lv_color_hex(0x3399FF);
    if (v <= 80) return lv_color_hex(0x4CAF50);
    if (v <= 150) return lv_color_hex(0xFF7A00);
    return lv_color_hex(0xFF5252);
}

static lv_color_t pm25_color(int v) {
    if (v < 0) return lv_color_hex(0x96969A);
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
    // env line shares the header slot: weather/PM ~10s, status line 5s
    int env_phase = (t.tm_sec % 15) < 10;
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
        if (env.pm10 < 0) strcpy(v, "-");
        set_env_span(1, v, pm10_color(env.pm10));
        set_env_span(2, " | PM2.5 ", lv_color_white());
        snprintf(v, sizeof(v), "%d", env.pm25);
        if (env.pm25 < 0) strcpy(v, "-");
        set_env_span(3, v, pm25_color(env.pm25));
        if (env.have_wx && env.temp >= 0) {
            set_env_span(4, " | ", lv_color_hex(0x96969A));
            snprintf(v, sizeof(v), "%dC ", env.temp);
            set_env_span(5, v, lv_color_hex(0x9BD8FF));
            const char* wname = "SKY";
            if (wcond == 0) wname = "SUN";
            else if (wcond == 1) wname = "CLOUD";
            else if (wcond == 2) wname = "OVC";
            else if (wcond == 3) wname = "RAIN";
            else if (wcond == 4) wname = "SNOW";
            else if (wcond == 5) wname = "SHWR";
            set_env_span(6, wname, wx_color(wcond));
        } else {
            set_env_span(4, "", lv_color_hex(0x96969A));
            set_env_span(5, "", lv_color_hex(0x96969A));
            set_env_span(6, "", lv_color_hex(0x96969A));
        }
        lv_obj_invalidate(env_group);
    } else {
        lv_obj_add_flag(env_group, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(hdr_label, buf);
        lv_color_t hdr_col = lv_color_white();
        if (fetch_fail) hdr_col = lv_color_hex(0xFF7A00);
        else if (!log_nvs || log_full) hdr_col = lv_color_hex(0xFFD400);
        lv_obj_set_style_text_color(hdr_label, hdr_col, 0);
    }

    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
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
            if (rs->has_arrival) rs->arrived_at = 0;  // fresh data supersedes
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
                    rs->zombie = 0;
                    rs->last_eta = 0;
                }
            }
            uint32_t row_age = (uint32_t)(now - rs->fetched);
            // exhausted countdown with a dead feed: honest "--" instead of
            // implying the bus is arriving (it may be stuck far away)
            if (rem == 0 && row_age > 120 && !rs->zombie) {
                lv_label_set_text(time1_labels[i], "--");
                lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0xFF7A00), 0);
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
                lv_color_t col = row_age < 60 ? lv_color_white()
                              : (row_age < 180 ? lv_color_hex(0xFFD400) : lv_color_hex(0xFF7A00));
                lv_obj_set_style_text_color(time1_labels[i], col, 0);
            }

            char sub[32] = "";
            if (rs->seats >= 0 && show_seats(ROWS[i].number)) {
                snprintf(sub, sizeof(sub), "(%d)", rs->seats);
            }
            lv_label_set_text(seats_labels[i], sub);
            sub[0] = 0;
            if (rs->arrtime2 >= 0) {
                int rem2 = rs->arrtime2 - (int)(now - rs->fetched);
                if (rem2 < 0) rem2 = 0;
                snprintf(sub, sizeof(sub), "+%dm", (rem2 + 59) / 60);
            }
            lv_label_set_text(time2_labels[i], sub);
            continue;
        }

        // no live arrival: schedule placeholder. A just-confirmed arrival
        // (the watched bus left the feed) keeps ARRIVING on screen briefly
        // instead of jumping straight to the next-bus estimate
        if (rs->arrived_at && now < rs->arrived_at + ARRIVING_SHOW_SECS) {
            lv_label_set_text(time1_labels[i], "ARRIVING");
            lv_obj_set_style_text_color(time1_labels[i], lv_color_white(), 0);
            lv_label_set_text(time2_labels[i], "");
            lv_label_set_text(seats_labels[i], "");
            rs->anim = 0;
            continue;
        }
        if (rc->has_sched) {
            int hw = we ? rc->hw_weekend : (is_peak_now() ? rc->hw_commute : rc->hw_weekday);
            int first_at_stop = rc->first_hm + rc->to_stop;
            int last_at_stop = rc->last_hm + rc->to_stop;
            if (hm < first_at_stop) {
                snprintf(b, sizeof(b), "STARTS %02d:%02d", first_at_stop / 60, first_at_stop % 60);
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
            lv_label_set_text(time2_labels[i], "");
            lv_label_set_text(seats_labels[i], "");
            rs->anim = 0;
            // short schedule blips keep disp so the live handoff animates;
            // long gaps (>5 min, e.g. overnight) snap to avoid a stale chase
            if (now - rs->fetched > 300) rs->disp = -1;
        } else {
            lv_label_set_text(time1_labels[i], "--");
            lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x505860), 0);
            lv_label_set_text(time2_labels[i], "");
            lv_label_set_text(seats_labels[i], "");
            rs->anim = 0;
            if (now - rs->fetched > 300) rs->disp = -1;
        }
    }
}

static void ui_tick_cb(lv_timer_t* t) { ui_update(); }

// 33ms chase timer (~30fps): when a row's target changes (feed update, next
// bus, regression), race the displayed seconds toward it so the numbers blur
// through the difference instead of jumping one frame
static void ui_anim_cb(lv_timer_t* t) {
    for (int i = 0; i < N_ROWS; i++) {
        row_state_t* rs = &rows[i];
        if (!rs->anim || rs->disp == rs->target)
            continue;
        int gap = rs->target - rs->disp;
        int step = (int)llabs((long)gap) * 3 / 10 + 1;  // ~30% of gap per tick
        if (step > 8) step = 8;                          // max 8s per 33ms (~240s/s)
        if (gap < 0) step = -step;
        rs->disp += step;
        if ((gap > 0 && rs->disp > rs->target) || (gap < 0 && rs->disp < rs->target))
            rs->disp = rs->target;
        char b[32];
        if (rs->disp == rs->target)
            fmt_secs(rs->target, b, sizeof(b));
        else
            snprintf(b, sizeof(b), "%02dm %02ds", rs->disp / 60, rs->disp % 60);
        lv_label_set_text(time1_labels[i], b);
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
            fetch_arrivals();
            if (fetch_fail) {
                refresh_secs = RETRY_REFRESH;
            } else {
                refresh_secs = (is_peak_now() || any_close_bus()) ? PEAK_REFRESH : OFF_REFRESH;
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

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_init();
    int ntp_tries = 0;
    while (kst_now() < 1600000000 && ntp_tries++ < 30) vTaskDelay(pdMS_TO_TICKS(500));
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
