// Thread-safe relay / schedule / override state machine.
// Every public function is safe to call from any task. All shared state is
// guarded by one recursive mutex; the change callback runs OUTSIDE the lock.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "app_priv.h"
#include "rtc_ds3231.h"

typedef enum { OVR_AUTO = 0, OVR_FORCE_ON = 1, OVR_FORCE_OFF = 2 } override_mode_t;
typedef enum { SRC_SCHEDULE, SRC_BUTTON, SRC_WEB, SRC_MATTER } change_src_t;

typedef struct { uint8_t on_h, on_m, off_h, off_m, enabled; } sched_slot_t;

typedef struct {
    bool            rtc_ok;
    bool            relay_on;
    bool            schedule_on;
    override_mode_t ovr;
    rtc_time_t      now;
    uint32_t        period_key;      // (start epoch-minute << 1) | schedule_on
    uint8_t         count;
    sched_slot_t    slots[MAX_SCHEDULES];
} sched_status_t;

typedef void (*relay_change_cb_t)(bool relay_on, change_src_t src);

// FIRST call in app_main(): relay OFF, mutex created. Needs no other init.
void      relay_scheduler_early_init(void);
// Loads NVS, starts the DS3231, starts the scheduler task.
esp_err_t relay_scheduler_start(void);
void      relay_scheduler_set_change_cb(relay_change_cb_t cb);

void      relay_scheduler_get_status(sched_status_t *out);
const char *relay_scheduler_override_name(override_mode_t m);
void      relay_scheduler_format_period(uint32_t key, char *buf, size_t size);

// Manual control. Return false (with *msg) if refused, e.g. RTC unavailable.
bool      relay_scheduler_set_override(override_mode_t m, change_src_t src, const char **msg);
bool      relay_scheduler_toggle(change_src_t src, const char **msg);

// ESP_OK, ESP_ERR_INVALID_ARG (bad input), ESP_FAIL (applied in RAM, NVS write failed).
esp_err_t relay_scheduler_set_schedules(const sched_slot_t *in, uint8_t n, const char **msg);
esp_err_t relay_scheduler_sync_rtc(const rtc_time_t *t, const char **msg);
bool      relay_scheduler_slot_valid(int on_h, int on_m, int off_h, int off_m);
