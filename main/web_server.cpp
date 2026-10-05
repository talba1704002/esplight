#include "web_server.h"
#include "app_priv.h"
#include "relay_scheduler.h"
#include "ap_manager.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web";
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

// ---------------- helpers ----------------
static esp_err_t send_json(httpd_req_t *r, const char *status, const char *body)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, body, HTTPD_RESP_USE_STRLEN);
}

// {"ok":..,"msg":".."} - msg strings are constants without quotes.
static esp_err_t send_result(httpd_req_t *r, int code, const char *msg)
{
    char buf[200];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}", code == 200 ? "true" : "false", msg);
    return send_json(r, code == 200 ? "200 OK" : code == 400 ? "400 Bad Request" : "500 Internal Server Error", buf);
}

static bool read_body(httpd_req_t *r, char *buf, size_t cap)
{
    if (r->content_len == 0 || r->content_len >= cap) return false;
    size_t got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, buf + got, r->content_len - got);
        if (n <= 0) return false;
        got += n;
    }
    buf[got] = 0;
    return true;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, nullptr, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static bool form_str(const char *body, const char *key, char *out, size_t cap)
{
    if (httpd_query_key_value(body, key, out, cap) != ESP_OK) return false;
    url_decode(out);
    return true;
}

static bool form_int(const char *body, const char *key, int *v)
{
    char t[8];
    if (httpd_query_key_value(body, key, t, sizeof(t)) != ESP_OK || !t[0]) return false;
    for (const char *p = t; *p; p++) if (*p < '0' || *p > '9') return false;
    *v = atoi(t);
    return true;
}

// strict "a,b,c,d,e": digits only (1-3 per field), nothing else
static bool parse_five(const char *s, int o[5])
{
    for (int i = 0; i < 5; i++) {
        int digits = 0, v = 0;
        while (*s >= '0' && *s <= '9') { if (++digits > 3) return false; v = v * 10 + (*s - '0'); s++; }
        if (!digits) return false;
        o[i] = v;
        if (i < 4) { if (*s != ',') return false; s++; }
    }
    return *s == 0;
}

static bool parse_list(char *str, sched_slot_t *out, uint8_t *n, const char **msg)
{
    *n = 0;
    size_t len = strlen(str);
    if (len == 0 || len > 250) { *msg = "Bad schedule list"; return false; }
    char *save = nullptr;
    // strtok_r would swallow empty tokens, so split manually to reject ";;"
    char *p = str;
    while (true) {
        char *semi = strchr(p, ';');
        if (semi) *semi = 0;
        if (*p == 0) { *msg = "Empty schedule entry"; return false; }
        if (*n >= MAX_SCHEDULES) { *msg = "Maximum 10 schedules"; return false; }
        int v[5];
        if (!parse_five(p, v)) { *msg = "Bad schedule format"; return false; }
        if (!relay_scheduler_slot_valid(v[0], v[1], v[2], v[3]) || v[4] > 1) { *msg = "Invalid times (ON and OFF must differ)"; return false; }
        out[*n] = {(uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3], (uint8_t)v[4]};
        (*n)++;
        if (!semi) break;
        p = semi + 1;
    }
    (void)save;
    return true;
}

// ---------------- handlers ----------------
static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_status(httpd_req_t *r)
{
    sched_status_t st;
    relay_scheduler_get_status(&st);

    char tm[24] = "RTC UNAVAILABLE", per[24] = "";
    unsigned sec = 0;
    if (st.rtc_ok) {
        snprintf(tm, sizeof(tm), "%04u-%02u-%02u %02u:%02u", st.now.year, st.now.month, st.now.day, st.now.hour, st.now.minute);
        sec = st.now.second;
        relay_scheduler_format_period(st.period_key, per, sizeof(per));
    }
    char list[MAX_SCHEDULES * 14 + 4]; size_t used = 0; list[0] = 0;
    for (uint8_t i = 0; i < st.count; i++) {
        int w = snprintf(list + used, sizeof(list) - used, "%s%u,%u,%u,%u,%u", i ? ";" : "",
                         st.slots[i].on_h, st.slots[i].on_m, st.slots[i].off_h, st.slots[i].off_m, st.slots[i].enabled);
        if (w < 0 || (size_t)w >= sizeof(list) - used) break;
        used += w;
    }
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"rtc\":%s,\"time\":\"%s\",\"sec\":%u,\"schedule\":\"%s\",\"relay\":\"%s\","
             "\"override\":\"%s\",\"period\":\"%s\",\"ap\":%s,\"list\":\"%s\"}",
             st.rtc_ok ? "true" : "false", tm, sec,
             st.rtc_ok ? (st.schedule_on ? "ON" : "OFF") : "UNKNOWN",
             st.relay_on ? "ON" : "OFF", relay_scheduler_override_name(st.ovr), per,
             ap_manager_is_on() ? "true" : "false", list);
    return send_json(r, "200 OK", buf);
}

static esp_err_t h_cmd(httpd_req_t *r)
{
    char body[64], c[12];
    if (!read_body(r, body, sizeof(body)) || !form_str(body, "c", c, sizeof(c))) return send_result(r, 400, "Missing command");
    for (char *p = c; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    override_mode_t m;
    if (!strcmp(c, "ON")) m = OVR_FORCE_ON;
    else if (!strcmp(c, "OFF")) m = OVR_FORCE_OFF;
    else if (!strcmp(c, "AUTO")) m = OVR_AUTO;
    else return send_result(r, 400, "Unknown command");
    const char *msg = "Error";
    bool ok = relay_scheduler_set_override(m, SRC_WEB, &msg);     // reports to Matter via the change callback
    return send_result(r, ok ? 200 : 400, msg);
}

static esp_err_t h_schedule(httpd_req_t *r)
{
    char body[420], list[260];
    if (!read_body(r, body, sizeof(body)) || !form_str(body, "list", list, sizeof(list))) return send_result(r, 400, "Missing schedule list");
    sched_slot_t slots[MAX_SCHEDULES]; uint8_t n = 0; const char *msg = "Error";
    if (!parse_list(list, slots, &n, &msg)) return send_result(r, 400, msg);
    esp_err_t e = relay_scheduler_set_schedules(slots, n, &msg);
    return send_result(r, e == ESP_OK ? 200 : (e == ESP_FAIL ? 500 : 400), msg);
}

static esp_err_t h_sync(httpd_req_t *r)
{
    char body[128]; int y, mo, d, h, mi, s;
    if (!read_body(r, body, sizeof(body)) || !form_int(body, "y", &y) || !form_int(body, "mo", &mo) || !form_int(body, "d", &d) ||
        !form_int(body, "h", &h) || !form_int(body, "mi", &mi) || !form_int(body, "s", &s))
        return send_result(r, 400, "Missing or invalid time fields");
    if (y > 9999 || mo > 99 || d > 99 || h > 99 || mi > 99 || s > 99) return send_result(r, 400, "Invalid phone date/time");
    rtc_time_t t = {(uint16_t)y, (uint8_t)mo, (uint8_t)d, (uint8_t)h, (uint8_t)mi, (uint8_t)s};
    const char *msg = "Error";
    esp_err_t e = relay_scheduler_sync_rtc(&t, &msg);
    return send_result(r, e == ESP_OK ? 200 : (e == ESP_FAIL ? 500 : 400), msg);
}

static esp_err_t h_adv_get(httpd_req_t *r)
{
    adv_settings_t a; ap_manager_get(&a);
    char buf[260];
    snprintf(buf, sizeof(buf),
             "{\"cpu\":%u,\"tx\":%u,\"ch\":%u,\"proto\":%u,\"mc\":%u,\"dbl\":%u,\"lng\":%u,\"ao\":%u,\"open\":%u,\"ssid\":\"%s\"}",
             a.cpu_mhz, a.tx_idx, a.channel, a.proto, a.max_clients, a.dbl_ms, a.long_ms, a.auto_off, a.pass[0] == 0, a.ssid);
    return send_json(r, "200 OK", buf);
}

static esp_err_t h_adv_set(httpd_req_t *r)
{
    char body[360];
    if (!read_body(r, body, sizeof(body))) return send_result(r, 400, "Missing or invalid fields");
    const char *msg = "Error"; esp_err_t e;
    char t[4];
    if (httpd_query_key_value(body, "reset", t, sizeof(t)) == ESP_OK) {
        e = ap_manager_reset_all(&msg);
    } else {
        adv_settings_t a; ap_manager_get(&a);
        int cpu = a.cpu_mhz, tx, ch, proto, mc, dbl, lng, ao = a.auto_off; char ssid[40], pass[72];
        if (!form_int(body, "tx", &tx) || !form_int(body, "ch", &ch) || !form_int(body, "proto", &proto) ||
            !form_int(body, "mc", &mc) || !form_int(body, "dbl", &dbl) || !form_int(body, "lng", &lng) ||
            !form_str(body, "ssid", ssid, sizeof(ssid)))
            return send_result(r, 400, "Missing or invalid fields");
        if (httpd_query_key_value(body, "cpu", t, sizeof(t)) == ESP_OK && !form_int(body, "cpu", &cpu)) return send_result(r, 400, "Invalid CPU value");
        if (httpd_query_key_value(body, "ao", t, sizeof(t)) == ESP_OK && !form_int(body, "ao", &ao)) return send_result(r, 400, "Invalid auto-off value");
        if (cpu > 1000 || tx > 255 || ch > 255 || proto > 255 || mc > 255 || dbl > 65535 || lng > 65535 || ao > 1) return send_result(r, 400, "Value out of range");
        a.cpu_mhz = cpu; a.tx_idx = tx; a.channel = ch; a.proto = proto; a.max_clients = mc; a.dbl_ms = dbl; a.long_ms = lng; a.auto_off = ao;
        strlcpy(a.ssid, ssid, sizeof(a.ssid));
        if (httpd_query_key_value(body, "open", t, sizeof(t)) == ESP_OK && t[0] == '1') {
            memset(a.pass, 0, sizeof(a.pass));
        } else if (form_str(body, "pass", pass, sizeof(pass)) && pass[0]) {
            if (strlen(pass) > 63) return send_result(r, 400, "Password too long (max 63)");
            memset(a.pass, 0, sizeof(a.pass)); strlcpy(a.pass, pass, sizeof(a.pass));
        }
        e = ap_manager_set(&a, &msg);
    }
    return send_result(r, e == ESP_OK ? 200 : (e == ESP_FAIL ? 500 : 400), msg);
}

esp_err_t web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 5;
    httpd_handle_t srv = nullptr;
    esp_err_t e = httpd_start(&srv, &cfg);
    if (e != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e)); return e; }

    static const httpd_uri_t uris[] = {
        {"/",             HTTP_GET,  h_root,     nullptr},
        {"/api/status",   HTTP_GET,  h_status,   nullptr},
        {"/api/cmd",      HTTP_POST, h_cmd,      nullptr},
        {"/api/schedule", HTTP_POST, h_schedule, nullptr},
        {"/api/sync",     HTTP_POST, h_sync,     nullptr},
        {"/api/adv",      HTTP_GET,  h_adv_get,  nullptr},
        {"/api/adv",      HTTP_POST, h_adv_set,  nullptr},
    };
    for (const auto &u : uris) httpd_register_uri_handler(srv, &u);
    ESP_LOGI(TAG, "Web portal on port 80 (AP and router network)");
    return ESP_OK;
}
