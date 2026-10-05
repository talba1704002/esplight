#include "relay_scheduler.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "sched";

// ---------------- shared state (guard: s_lock) ----------------
static SemaphoreHandle_t s_lock;
static sched_slot_t      s_slots[MAX_SCHEDULES];
static uint8_t           s_count = 1;
static override_mode_t   s_ovr = OVR_AUTO;
static bool              s_relay = false, s_sched_on = false, s_rtc_ok = false;
static bool              s_last_notified = false, s_fail_logged = false;
static rtc_time_t        s_now = {2000, 1, 1, 0, 0, 0};
static uint32_t          s_period = 0;
static int64_t           s_recover_at = 0;
static relay_change_cb_t s_cb = nullptr;

#define LOCK()   xSemaphoreTakeRecursive(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGiveRecursive(s_lock)

static const TickType_t RTC_READ_INTERVAL = pdMS_TO_TICKS(500);
static const int64_t    RTC_RECOVERY_MS   = 5000;
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

// ---------------- relay ----------------
static void set_relay(bool on)
{
    s_relay = on;
    bool level = RELAY_ACTIVE_HIGH ? on : !on;
    gpio_set_level(PIN_RELAY, level ? 1 : 0);
}

void relay_scheduler_early_init(void)
{
    const int off = RELAY_ACTIVE_HIGH ? 0 : 1;
    gpio_set_level(PIN_RELAY, off);                 // preset OFF level
    gpio_set_direction(PIN_RELAY, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(PIN_RELAY, off);
    s_relay = false;
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
}

// Fires the callback (outside the lock) when the relay differs from the last report.
static void notify_if_changed(change_src_t src)
{
    LOCK();
    bool r = s_relay;
    bool changed = (r != s_last_notified);
    if (changed) s_last_notified = r;
    UNLOCK();
    if (changed && s_cb) s_cb(r, src);
}

// ---------------- time helpers ----------------
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t yy = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

static uint32_t to_epoch_min(const rtc_time_t &t)
{
    return (uint32_t)(days_from_civil(t.year, t.month, t.day) * 1440 + t.hour * 60 + t.minute);
}

// ---------------- schedule evaluation ----------------
bool relay_scheduler_slot_valid(int onH, int onM, int offH, int offM)
{
    if (onH < 0 || onH > 23 || offH < 0 || offH > 23) return false;
    if (onM < 0 || onM > 59 || offM < 0 || offM > 59) return false;
    return onH * 60 + onM != offH * 60 + offM;
}

static bool slot_contains(const sched_slot_t &s, int m)
{
    int on = s.on_h * 60 + s.on_m, off = s.off_h * 60 + s.off_m;
    if (on < off) return m >= on && m < off;
    return m >= on || m < off;                      // window crosses midnight
}

static bool state_at(int m)                         // union of all enabled windows
{
    m = ((m % 1440) + 1440) % 1440;
    for (uint8_t i = 0; i < s_count; i++)
        if (s_slots[i].enabled && slot_contains(s_slots[i], m)) return true;
    return false;
}

// Period = interval since the last real change of the COMBINED state.
// No real transition (no slots / 24 h coverage) -> boundary is today's midnight.
static uint32_t period_key(const rtc_time_t &t)
{
    int nowMin = t.hour * 60 + t.minute, best = -1;
    for (uint8_t i = 0; i < s_count; i++) {
        if (!s_slots[i].enabled) continue;
        int ev[2] = {s_slots[i].on_h * 60 + s_slots[i].on_m, s_slots[i].off_h * 60 + s_slots[i].off_m};
        for (int k = 0; k < 2; k++) {
            if (state_at(ev[k]) == state_at(ev[k] - 1)) continue;
            int ago = (nowMin - ev[k] + 1440) % 1440;
            if (best < 0 || ago < best) best = ago;
        }
    }
    uint32_t nowEp = to_epoch_min(t);
    uint32_t start = (best < 0) ? nowEp - nowMin : nowEp - best;
    return start * 2 + (state_at(nowMin) ? 1 : 0);
}

void relay_scheduler_format_period(uint32_t key, char *buf, size_t size)
{
    uint32_t start = key >> 1;
    int y, m, d;
    civil_from_days(start / 1440, &y, &m, &d);
    int mm = start % 1440;
    snprintf(buf, size, "%04d%02d%02d_%02d%02d_%c", y, m, d, mm / 60, mm % 60, (key & 1) ? 'N' : 'F');
}

// ---------------- NVS ----------------
static const size_t SCHED_BLOB = 1 + MAX_SCHEDULES * 5;

static void use_default_schedule(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    s_slots[0] = {DEFAULT_ON_H, DEFAULT_ON_M, DEFAULT_OFF_H, DEFAULT_OFF_M, 1};
    s_count = 1;
}

static void load_schedule(void)
{
    use_default_schedule();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) { ESP_LOGW(TAG, "NVS open failed -> default schedule"); return; }
    uint8_t blob[SCHED_BLOB];
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, "sched", blob, &len) == ESP_OK && len == SCHED_BLOB) {
        uint8_t n = blob[0];
        bool valid = (n >= 1 && n <= MAX_SCHEDULES);
        for (uint8_t i = 0; valid && i < n; i++) {
            const uint8_t *p = &blob[1 + i * 5];
            if (!relay_scheduler_slot_valid(p[0], p[1], p[2], p[3]) || p[4] > 1) valid = false;
        }
        if (valid) {
            memset(s_slots, 0, sizeof(s_slots));
            for (uint8_t i = 0; i < n; i++) {
                const uint8_t *p = &blob[1 + i * 5];
                s_slots[i] = {p[0], p[1], p[2], p[3], p[4]};
            }
            s_count = n;
        } else {
            ESP_LOGW(TAG, "Invalid saved schedules -> defaults");
        }
    }
    nvs_close(h);
}

static bool save_schedule(void)                      // caller holds the lock
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t blob[SCHED_BLOB], old[SCHED_BLOB];
    memset(blob, 0, sizeof(blob));
    blob[0] = s_count;
    for (uint8_t i = 0; i < s_count; i++) {
        uint8_t *p = &blob[1 + i * 5];
        p[0] = s_slots[i].on_h; p[1] = s_slots[i].on_m;
        p[2] = s_slots[i].off_h; p[3] = s_slots[i].off_m; p[4] = s_slots[i].enabled;
    }
    size_t len = sizeof(old);
    bool ok = true;
    if (nvs_get_blob(h, "sched", old, &len) != ESP_OK || len != SCHED_BLOB || memcmp(old, blob, SCHED_BLOB) != 0)
        ok = (nvs_set_blob(h, "sched", blob, SCHED_BLOB) == ESP_OK) && (nvs_commit(h) == ESP_OK);   // flash-wear guard
    nvs_close(h);
    return ok;
}

static bool erase_override(void)                     // NVS only, RAM untouched
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e1 = nvs_erase_key(h, "mode"), e2 = nvs_erase_key(h, "per_key");
    bool ok = (e1 == ESP_OK || e1 == ESP_ERR_NVS_NOT_FOUND) && (e2 == ESP_OK || e2 == ESP_ERR_NVS_NOT_FOUND);
    if (e1 == ESP_OK || e2 == ESP_OK) ok = ok && (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

static void clear_stored_override(void) { s_ovr = OVR_AUTO; erase_override(); }

// Persist only during an ON schedule period (same policy as v13).
static bool save_override(void)
{
    if (!s_sched_on) return erase_override();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t mode = 255; uint32_t key = 0;
    nvs_get_u8(h, "mode", &mode);
    nvs_get_u32(h, "per_key", &key);
    bool ok = true, dirty = false;
    if (mode != (uint8_t)s_ovr) { ok &= (nvs_set_u8(h, "mode", (uint8_t)s_ovr) == ESP_OK); dirty = true; }
    if (key != s_period)        { ok &= (nvs_set_u32(h, "per_key", s_period) == ESP_OK); dirty = true; }
    if (dirty) ok &= (nvs_commit(h) == ESP_OK);
    nvs_close(h);
    return ok;
}

static void load_override(void)                      // only restores the CURRENT period's override
{
    s_ovr = OVR_AUTO;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t mode = OVR_AUTO; uint32_t key = 0;
    bool haveKey = (nvs_get_u32(h, "per_key", &key) == ESP_OK);
    nvs_get_u8(h, "mode", &mode);
    nvs_close(h);

    if (!s_sched_on) { if (mode != OVR_AUTO || haveKey) clear_stored_override(); return; }
    if (mode == OVR_AUTO && !haveKey) return;
    if (mode > OVR_FORCE_OFF || mode == OVR_AUTO || !haveKey || key != s_period) { clear_stored_override(); return; }
    s_ovr = (override_mode_t)mode;
    ESP_LOGI(TAG, "Current-period override restored");
}

// ---------------- core logic (lock held) ----------------
static void apply_state(void)
{
    if (s_ovr == OVR_FORCE_ON) set_relay(true);
    else if (s_ovr == OVR_FORCE_OFF) set_relay(false);
    else set_relay(s_sched_on);
}

// A new period ALWAYS ends any manual override.
static void handle_schedule_change(const rtc_time_t &t)
{
    uint32_t key = period_key(t);
    s_sched_on = (key & 1);
    if (key == s_period) return;
    ESP_LOGI(TAG, "New schedule period");
    s_period = key;
    bool had = (s_ovr != OVR_AUTO);
    clear_stored_override();
    if (had) ESP_LOGI(TAG, "Manual override expired -> AUTO");
    set_relay(s_sched_on);
}

static void mark_rtc_failed(const char *reason)
{
    bool was = s_rtc_ok;
    s_rtc_ok = false;
    set_relay(false);
    if (was) {
        ESP_LOGE(TAG, "RTC FAILURE: %s -> relay forced OFF", reason);
        s_recover_at = now_ms();
        s_fail_logged = false;
    }
}

static void start_with_trusted_time(const rtc_time_t &t)
{
    s_now = t;
    s_period = period_key(t);
    s_sched_on = (s_period & 1);
    s_rtc_ok = true;
    s_fail_logged = false;
    load_override();
    apply_state();
    char p[24]; relay_scheduler_format_period(s_period, p, sizeof(p));
    ESP_LOGI(TAG, "State: schedule %s, override %s, relay %s, period %s",
             s_sched_on ? "ON" : "OFF", relay_scheduler_override_name(s_ovr), s_relay ? "ON" : "OFF", p);
}

static bool refresh_trusted_time(void)               // fresh RTC read for manual actions
{
    if (!s_rtc_ok) return false;
    rtc_time_t t; const char *why = "";
    if (rtc_ds3231_read(&t, &why) != ESP_OK) { mark_rtc_failed(why); return false; }
    s_now = t;
    handle_schedule_change(t);
    return true;
}

static bool commit_override(override_mode_t m)
{
    if (m == OVR_AUTO) {
        s_ovr = OVR_AUTO;
        bool ok = erase_override();
        s_sched_on = state_at(s_now.hour * 60 + s_now.minute);
        apply_state();
        return ok;
    }
    s_ovr = m;
    apply_state();
    return save_override();
}

// ---------------- public API ----------------
const char *relay_scheduler_override_name(override_mode_t m)
{
    switch (m) { case OVR_AUTO: return "AUTO"; case OVR_FORCE_ON: return "FORCE_ON"; case OVR_FORCE_OFF: return "FORCE_OFF"; }
    return "UNKNOWN";
}

void relay_scheduler_set_change_cb(relay_change_cb_t cb) { s_cb = cb; }

void relay_scheduler_get_status(sched_status_t *o)
{
    LOCK();
    o->rtc_ok = s_rtc_ok; o->relay_on = s_relay; o->schedule_on = s_sched_on;
    o->ovr = s_ovr; o->now = s_now; o->period_key = s_period; o->count = s_count;
    memcpy(o->slots, s_slots, sizeof(s_slots));
    UNLOCK();
}

bool relay_scheduler_set_override(override_mode_t m, change_src_t src, const char **msg)
{
    LOCK();
    if (!refresh_trusted_time()) {
        UNLOCK(); notify_if_changed(src);
        *msg = "RTC unavailable";
        return false;
    }
    bool saved = commit_override(m);
    UNLOCK();
    notify_if_changed(src);
    if (m == OVR_AUTO)         *msg = saved ? "AUTO mode" : "AUTO mode (NVS clear failed)";
    else if (m == OVR_FORCE_ON) *msg = saved ? "Forced ON" : "Forced ON (NVS save failed)";
    else                        *msg = saved ? "Forced OFF" : "Forced OFF (NVS save failed)";
    return true;
}

bool relay_scheduler_toggle(change_src_t src, const char **msg)
{
    LOCK();
    if (!refresh_trusted_time()) {
        UNLOCK(); notify_if_changed(src);
        *msg = "RTC unavailable";
        return false;
    }
    bool saved = commit_override(s_relay ? OVR_FORCE_OFF : OVR_FORCE_ON);
    UNLOCK();
    notify_if_changed(src);
    *msg = saved ? "Toggled" : "Toggled (NVS save failed)";
    return true;
}

esp_err_t relay_scheduler_set_schedules(const sched_slot_t *in, uint8_t n, const char **msg)
{
    if (n < 1 || n > MAX_SCHEDULES) { *msg = "Need 1 to 10 schedules"; return ESP_ERR_INVALID_ARG; }
    for (uint8_t i = 0; i < n; i++)
        if (!relay_scheduler_slot_valid(in[i].on_h, in[i].on_m, in[i].off_h, in[i].off_m) || in[i].enabled > 1) {
            *msg = "Invalid times (ON and OFF must differ)";
            return ESP_ERR_INVALID_ARG;
        }
    LOCK();
    memset(s_slots, 0, sizeof(s_slots));
    memcpy(s_slots, in, n * sizeof(sched_slot_t));
    s_count = n;
    bool saved = save_schedule();
    bool rtcOk = refresh_trusted_time();             // re-evaluates with the new slots
    UNLOCK();
    notify_if_changed(SRC_WEB);
    if (!saved) { *msg = "Schedules applied but NVS save FAILED"; return ESP_FAIL; }
    *msg = rtcOk ? "Schedules saved" : "Schedules saved (applies when RTC is back)";
    return ESP_OK;
}

esp_err_t relay_scheduler_sync_rtc(const rtc_time_t *t, const char **msg)
{
    if (!rtc_ds3231_valid(t)) { *msg = "Invalid phone date/time"; return ESP_ERR_INVALID_ARG; }
    LOCK();
    if (rtc_ds3231_write(t) != ESP_OK) { UNLOCK(); *msg = "DS3231 not responding"; return ESP_FAIL; }
    rtc_time_t v; const char *why = "";
    if (rtc_ds3231_read(&v, &why) != ESP_OK) {
        if (s_rtc_ok) mark_rtc_failed(why);
        UNLOCK(); notify_if_changed(SRC_WEB);
        *msg = "RTC write/readback failed";
        return ESP_FAIL;
    }
    if (!s_rtc_ok) start_with_trusted_time(v);
    else { s_now = v; handle_schedule_change(v); }
    UNLOCK();
    notify_if_changed(SRC_WEB);
    ESP_LOGI(TAG, "RTC synchronized from phone");
    *msg = "RTC synchronized";
    return ESP_OK;
}

// ---------------- scheduler task ----------------
static void scheduler_task(void *)
{
    for (;;) {
        vTaskDelay(RTC_READ_INTERVAL);

        LOCK(); bool ok = s_rtc_ok; int64_t since = now_ms() - s_recover_at; UNLOCK();

        if (ok) {
            rtc_time_t t; const char *why = "";
            esp_err_t e = rtc_ds3231_read(&t, &why);          // I2C outside the lock
            LOCK();
            if (e != ESP_OK) mark_rtc_failed(why);
            else if (s_rtc_ok) { s_now = t; handle_schedule_change(t); }
            UNLOCK();
        } else if (since >= RTC_RECOVERY_MS) {
            LOCK(); s_recover_at = now_ms(); bool logged = s_fail_logged; UNLOCK();
            if (!logged) ESP_LOGW(TAG, "Attempting I2C bus recovery...");
            rtc_ds3231_recover_bus();
            rtc_time_t t; const char *why = "";
            LOCK();
            if (rtc_ds3231_read(&t, &why) == ESP_OK) { start_with_trusted_time(t); ESP_LOGI(TAG, "RTC recovered"); }
            else {
                set_relay(false);
                if (!s_fail_logged) { ESP_LOGE(TAG, "RTC recovery failed: %s (retrying every 5 s silently)", why); s_fail_logged = true; }
            }
            UNLOCK();
        }
        notify_if_changed(SRC_SCHEDULE);
    }
}

esp_err_t relay_scheduler_start(void)
{
    if (!s_lock) relay_scheduler_early_init();
    load_schedule();
    esp_err_t e = rtc_ds3231_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(e));

    rtc_time_t t = {}; const char *why = "RTC init failed";
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = (e == ESP_OK) && rtc_ds3231_read(&t, &why) == ESP_OK;
        if (!ok) vTaskDelay(pdMS_TO_TICKS(100));
    }
    LOCK();
    if (ok) start_with_trusted_time(t);
    else {
        ESP_LOGE(TAG, "%s -> relay OFF, sync the RTC from the web UI or wait for recovery", why);
        s_rtc_ok = false; s_recover_at = now_ms(); set_relay(false);
    }
    UNLOCK();
    if (xTaskCreate(scheduler_task, "sched", 4096, nullptr, 5, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
