#include "ap_manager.h"
#include "app_priv.h"
#include "button_handler.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_pm.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "ap";
static const uint8_t ADV_MAGIC = 0xA7;
// quarter-dBm values for esp_wifi_set_max_tx_power(): 2,5,7,8.5,11,13,15,17,18.5,19,19.5 dBm
static const int8_t TX_QDBM[TX_LEVEL_COUNT] = {8, 20, 28, 34, 44, 52, 60, 68, 74, 76, 78};

static SemaphoreHandle_t s_mtx;
static adv_settings_t    s_adv;
static esp_netif_t      *s_ap_netif;
static volatile bool     s_req_toggle, s_req_reset, s_req_restart;
static bool              s_desired = AP_DEFAULT_ON, s_running = false;
static int64_t           s_last_try = 0, s_last_seen = 0;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void adv_defaults(adv_settings_t *a)
{
    memset(a, 0, sizeof(*a));
    a->magic = ADV_MAGIC; a->tx_idx = 3; a->channel = 1; a->proto = 1;
    a->max_clients = 1; a->auto_off = 0; a->cpu_mhz = 160; a->dbl_ms = 300; a->long_ms = 2000;
    strlcpy(a->ssid, AP_DEFAULT_SSID, sizeof(a->ssid));
}

static bool text_ok(const char *t, size_t mn, size_t mx, bool quotes)
{
    size_t n = strnlen(t, 80);
    if (n < mn || n > mx) return false;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (c < 32 || c > 126) return false;
        if (!quotes && (c == '"' || c == '\\')) return false;
    }
    return true;
}

static bool adv_validate(const adv_settings_t *a, const char **msg)
{
    if (a->tx_idx >= TX_LEVEL_COUNT)              { *msg = "Invalid TX power"; return false; }
    if (a->channel < 1 || a->channel > 13)        { *msg = "Channel must be 1 to 13"; return false; }
    if (a->proto > 2)                             { *msg = "Invalid Wi-Fi mode"; return false; }
    if (a->max_clients < 1 || a->max_clients > 4) { *msg = "Max clients must be 1 to 4"; return false; }
    if (a->auto_off > 1)                          { *msg = "Invalid auto-off value"; return false; }
    if (a->cpu_mhz != 160 && a->cpu_mhz != 240)   { *msg = "CPU must be 160 or 240 MHz"; return false; }
    if (a->dbl_ms < 200 || a->dbl_ms > 600)       { *msg = "Double-click window 200 to 600 ms"; return false; }
    if (a->long_ms < 1000 || a->long_ms > 5000)   { *msg = "Long press 1000 to 5000 ms"; return false; }
    if (!text_ok(a->ssid, 1, 32, false))          { *msg = "SSID: 1-32 plain characters, no quote or backslash"; return false; }
    if (a->pass[0] && !text_ok(a->pass, 8, 63, true)) { *msg = "Password: empty (open) or 8 to 63 plain characters"; return false; }
    return true;
}

// Fixed CPU clock (min == max, no light sleep). Needs CONFIG_PM_ENABLE=y.
static void apply_cpu(uint16_t mhz)
{
    esp_pm_config_t cfg = {};
    cfg.max_freq_mhz = mhz;
    cfg.min_freq_mhz = mhz;
    cfg.light_sleep_enable = false;
    esp_err_t e = esp_pm_configure(&cfg);
    if (e == ESP_OK) ESP_LOGI(TAG, "CPU set to %u MHz", (unsigned)mhz);
    else ESP_LOGW(TAG, "CPU change failed: %s", esp_err_to_name(e));
}

// ---- NVS ----
static bool save_adv(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    adv_settings_t old; size_t len = sizeof(old);
    bool ok = true;
    if (nvs_get_blob(h, "adv2", &old, &len) != ESP_OK || len != sizeof(old) || memcmp(&old, &s_adv, sizeof(old)) != 0)
        ok = (nvs_set_blob(h, "adv2", &s_adv, sizeof(s_adv)) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

static bool save_ap_state(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t v = 255; nvs_get_u8(h, "ap_on", &v);
    bool ok = true;
    if (v != (on ? 1 : 0)) ok = (nvs_set_u8(h, "ap_on", on ? 1 : 0) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

esp_err_t ap_manager_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    adv_defaults(&s_adv);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        adv_settings_t tmp; size_t len = sizeof(tmp);
        if (nvs_get_blob(h, "adv2", &tmp, &len) == ESP_OK && len == sizeof(tmp)) {
            tmp.ssid[sizeof(tmp.ssid) - 1] = 0; tmp.pass[sizeof(tmp.pass) - 1] = 0;
            const char *m = "";
            if (tmp.magic == ADV_MAGIC && adv_validate(&tmp, &m)) s_adv = tmp;
            else ESP_LOGW(TAG, "Invalid saved advanced settings -> defaults");
        }
        uint8_t v = 255;
        if (nvs_get_u8(h, "ap_on", &v) == ESP_OK && v <= 1) s_desired = (v == 1);
        nvs_close(h);
    }
    apply_cpu(s_adv.cpu_mhz);
    return ESP_OK;
}

void ap_manager_get(adv_settings_t *o) { xSemaphoreTake(s_mtx, portMAX_DELAY); *o = s_adv; xSemaphoreGive(s_mtx); }
bool ap_manager_is_on(void) { return s_running; }

// ---- AP control (AP task only) ----
static esp_err_t ap_start(void)
{
    adv_settings_t a; ap_manager_get(&a);
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_mode_t m;
    esp_err_t e = esp_wifi_get_mode(&m);
    if (e != ESP_OK) return e;                                   // Wi-Fi not initialised yet: retry later
    e = esp_wifi_set_mode((m == WIFI_MODE_STA || m == WIFI_MODE_APSTA) ? WIFI_MODE_APSTA : WIFI_MODE_AP);
    if (e != ESP_OK) return e;

    wifi_config_t c = {};
    strlcpy((char *)c.ap.ssid, a.ssid, sizeof(c.ap.ssid));
    c.ap.ssid_len = strlen(a.ssid);
    c.ap.channel = a.channel;
    c.ap.max_connection = a.max_clients;
    c.ap.authmode = a.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    strlcpy((char *)c.ap.password, a.pass, sizeof(c.ap.password));
    e = esp_wifi_set_config(WIFI_IF_AP, &c);
    if (e != ESP_OK) return e;

    uint8_t mask = WIFI_PROTOCOL_11B;
    if (a.proto >= 1) mask |= WIFI_PROTOCOL_11G;
    if (a.proto >= 2) mask |= WIFI_PROTOCOL_11N;
    esp_wifi_set_protocol(WIFI_IF_AP, mask);
    esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_max_tx_power(TX_QDBM[a.tx_idx]);               // global: also applies to the Matter STA link

    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(s_ap_netif, &ip);
    ESP_LOGI(TAG, "AP ON: \"%s\" (%s) ip " IPSTR, a.ssid, a.pass[0] ? "secured" : "open", IP2STR(&ip.ip));
    s_running = true;
    s_last_seen = now_ms();
    return ESP_OK;
}

static void ap_stop(void)
{
    wifi_mode_t m;
    if (esp_wifi_get_mode(&m) == ESP_OK && (m == WIFI_MODE_AP || m == WIFI_MODE_APSTA))
        esp_wifi_set_mode(WIFI_MODE_STA);                        // Matter keeps the station side
    s_running = false;
    ESP_LOGI(TAG, "AP OFF");
}

static void ap_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t now = now_ms();

        if (s_req_reset) {                                       // triple click: AP fields back to defaults
            s_req_reset = false;
            adv_settings_t d; adv_defaults(&d);
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_adv.tx_idx = d.tx_idx; s_adv.channel = d.channel; s_adv.proto = d.proto;
            s_adv.max_clients = d.max_clients; s_adv.auto_off = d.auto_off;
            memcpy(s_adv.ssid, d.ssid, sizeof(d.ssid)); memcpy(s_adv.pass, d.pass, sizeof(d.pass));
            bool ok = save_adv();
            xSemaphoreGive(s_mtx);
            s_desired = true; save_ap_state(true);
            if (s_running) ap_stop();
            s_last_try = 0;
            ESP_LOGI(TAG, "AP reset to defaults (SSID %s, open)%s", AP_DEFAULT_SSID, ok ? "" : " - NVS save FAILED");
        }
        if (s_req_toggle) {                                      // double click
            s_req_toggle = false;
            s_desired = !s_desired;
            if (!save_ap_state(s_desired)) ESP_LOGW(TAG, "AP state NOT persisted");
            if (!s_desired && s_running) ap_stop();
            s_last_try = 0;
        }
        if (s_req_restart) {                                     // new SSID/password/channel from the web UI
            s_req_restart = false;
            if (s_running) { ap_stop(); vTaskDelay(pdMS_TO_TICKS(300)); }
            s_last_try = 0;
        }

        if (s_desired && !s_running && now - s_last_try >= 5000) {
            s_last_try = now;
            esp_err_t e = ap_start();
            if (e != ESP_OK) ESP_LOGW(TAG, "AP start failed (%s), retrying", esp_err_to_name(e));
        } else if (!s_desired && s_running) {
            ap_stop();
        }

        adv_settings_t a; ap_manager_get(&a);
        if (s_running && a.auto_off) {                           // idle auto-off, saved state untouched
            wifi_sta_list_t l;
            if (esp_wifi_ap_get_sta_list(&l) == ESP_OK && l.num > 0) s_last_seen = now;
            else if (now - s_last_seen >= 120000) { ESP_LOGI(TAG, "No phone for 2 min -> AP auto OFF"); ap_stop(); s_desired = false; }
        }
    }
}

esp_err_t ap_manager_start(void)
{
    return xTaskCreate(ap_task, "ap_mgr", 4096, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ap_manager_set(const adv_settings_t *in, const char **msg)
{
    adv_settings_t n = *in;
    n.magic = ADV_MAGIC;
    n.ssid[sizeof(n.ssid) - 1] = 0; n.pass[sizeof(n.pass) - 1] = 0;
    if (!adv_validate(&n, msg)) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool restart = n.channel != s_adv.channel || n.proto != s_adv.proto || n.max_clients != s_adv.max_clients ||
                   strcmp(n.ssid, s_adv.ssid) != 0 || strcmp(n.pass, s_adv.pass) != 0;
    s_adv = n;
    bool saved = save_adv();
    xSemaphoreGive(s_mtx);

    button_handler_set_timing(n.dbl_ms, n.long_ms);              // timing applies at once
    apply_cpu(n.cpu_mhz);
    esp_wifi_set_max_tx_power(TX_QDBM[n.tx_idx]);
    if (restart) s_req_restart = true;                           // after the HTTP reply was sent

    if (!saved) { *msg = "Applied but NVS save FAILED"; return ESP_FAIL; }
    *msg = (restart && s_running) ? "Saved. Wi-Fi restarting - reconnect to the new settings" : "Saved";
    return ESP_OK;
}

esp_err_t ap_manager_reset_all(const char **msg)
{
    adv_settings_t d; adv_defaults(&d);
    return ap_manager_set(&d, msg);
}

void ap_manager_request_toggle(void)   { s_req_toggle = true; }
void ap_manager_request_ap_reset(void) { s_req_reset = true; }
