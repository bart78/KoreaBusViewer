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
    time_t fetched;
} row_state_t;
static row_state_t rows[N_ROWS];

static time_t last_refresh = 0;
static int refresh_secs = 60;
static time_t last_good = 0;
static int fetch_fail = 0;

// ---- LVGL objects ----
static lv_obj_t* hdr_label;
static lv_obj_t* clock_label;
static lv_obj_t* route_labels[N_ROWS];
static lv_obj_t* time1_labels[N_ROWS];
static lv_obj_t* time2_labels[N_ROWS];

static time_t kst_now(void) { return time(NULL) + 9 * 3600; }

static void fmt_secs(int s, char* out, size_t n) {
    if (s < 60) { snprintf(out, n, "SOON"); return; }
    snprintf(out, n, "%dm %02ds", s / 60, s % 60);
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

        // confirmed arrival: a close bus (<=240s) vanished from the feed —
        // log its last predicted arrival moment (type 1, always logged)
        if (old_has && !rows[r].has_arrival && old_eta <= 240)
            log_arrival(r, old_fetch + old_eta, 1);

        // predicted arrival: first bus crosses below 90s (per vehicle, so a
        // second close bus is not suppressed by the first)
        if (rows[r].has_arrival) {
            if (rows[r].arrtime <= 90) {
                if (!rows[r].logged || rows[r].veh_logged != rows[r].veh) {
                    log_arrival(r, now + rows[r].arrtime, 0);
                    rows[r].logged = 1;
                    rows[r].veh_logged = rows[r].veh;
                }
            } else if (rows[r].arrtime > 300) {
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
// type 0 = predicted arrival (crossed <=90s), type 1 = confirmed (vanished from feed)
#define LOG_DAYS_KEEP  60
#define LOG_DAY_BYTES  1024

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
    uint16_t ev = (uint16_t)((type << 14) | (route_idx << 11) | (minute & 0x7FF));
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

    hdr_label = lv_label_create(scr);
    lv_obj_set_style_text_color(hdr_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(hdr_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(hdr_label, "STOP: 07593");
    lv_obj_set_pos(hdr_label, 10, 6);

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
        lv_obj_set_style_text_font(time1_labels[i], &lv_font_montserrat_28, 0);
        lv_obj_set_pos(time1_labels[i], 160, y);

        time2_labels[i] = lv_label_create(scr);
        lv_obj_set_style_text_color(time2_labels[i], lv_color_hex(0x96969A), 0);
        lv_obj_set_style_text_font(time2_labels[i], &lv_font_montserrat_16, 0);
        lv_obj_set_pos(time2_labels[i], 160, y + 36);

        char no[8];
        snprintf(no, sizeof(no), "%d", ROWS[i].number);
        lv_label_set_text(route_labels[i], no);
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

static int weekend_p(void) {
    time_t now = kst_now();
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_wday == 0 || t.tm_wday == 6);
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
    lv_label_set_text(hdr_label, buf);
    lv_color_t hdr_col = lv_color_white();
    if (fetch_fail) hdr_col = lv_color_hex(0xFF7A00);
    else if (!log_nvs || log_full) hdr_col = lv_color_hex(0xFFD400);
    lv_obj_set_style_text_color(hdr_label, hdr_col, 0);

    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(clock_label, buf);

    int hm = now_hm();
    int we = weekend_p();
    for (int i = 0; i < N_ROWS; i++) {
        const row_cfg_t* rc = &ROWS[i];
        row_state_t* rs = &rows[i];
        char b[32];

        if (rs->has_arrival) {
            int rem = rs->arrtime - (int)(now - rs->fetched);
            if (rem < 0) rem = 0;
            fmt_secs(rem, b, sizeof(b));
            lv_label_set_text(time1_labels[i], b);
            uint32_t row_age = (uint32_t)(now - rs->fetched);
            lv_color_t col = row_age < 60 ? lv_color_white()
                          : (row_age < 180 ? lv_color_hex(0xFFD400) : lv_color_hex(0xFF7A00));
            lv_obj_set_style_text_color(time1_labels[i], col, 0);

            char sub[32] = "";
            if (rs->seats >= 0 && show_seats(ROWS[i].number)) {
                snprintf(sub, sizeof(sub), "SEATS %d", rs->seats);
            } else if (rs->arrtime2 >= 0) {
                int rem2 = rs->arrtime2 - (int)(now - rs->fetched);
                if (rem2 < 0) rem2 = 0;
                snprintf(sub, sizeof(sub), "+%dm", (rem2 + 59) / 60);
            }
            lv_label_set_text(time2_labels[i], sub);
            continue;
        }

        // no live arrival: schedule placeholder
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
        } else {
            lv_label_set_text(time1_labels[i], "--");
            lv_obj_set_style_text_color(time1_labels[i], lv_color_hex(0x505860), 0);
            lv_label_set_text(time2_labels[i], "");
        }
    }
}

static void ui_tick_cb(lv_timer_t* t) { ui_update(); }

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
                refresh_secs = is_peak_now() ? PEAK_REFRESH : OFF_REFRESH;
            }
            last_refresh = now;
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

    bsp_display_lock(0);
    ui_build();
    ui_update();
    lv_timer_create(ui_tick_cb, 1000, NULL);
    bsp_display_unlock();

    last_refresh = kst_now() - refresh_secs;

    xTaskCreatePinnedToCore(refresh_task, "refresh", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "bus board running");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
