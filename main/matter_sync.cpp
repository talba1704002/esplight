#include "matter_sync.h"
#include "relay_scheduler.h"
#include <esp_matter.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

using namespace esp_matter;
using namespace chip::app::Clusters;

static const char *TAG = "matter_sync";
static uint16_t s_ep = 0;
static TaskHandle_t s_task = nullptr;
static volatile TaskHandle_t s_updating = nullptr;

bool matter_sync_is_internal_update(void)
{
    return s_updating != nullptr && s_updating == xTaskGetCurrentTaskHandle();
}

static bool current_relay(void)
{
    sched_status_t st;
    relay_scheduler_get_status(&st);
    return st.relay_on;
}

static void push_attribute(bool on)
{
    esp_matter_attr_val_t val = esp_matter_bool(on);
    s_updating = xTaskGetCurrentTaskHandle();
    // update() takes the CHIP stack lock itself when called from a non-Matter task,
    // writes the attribute and schedules a report to all subscribers.
    esp_err_t e = attribute::update(s_ep, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
    s_updating = nullptr;
    if (e != ESP_OK) ESP_LOGW(TAG, "attribute update failed: %s", esp_err_to_name(e));
}

void matter_sync_force_report(void)
{
    esp_matter_attr_val_t val = esp_matter_bool(current_relay());
    lock::status_t ls = lock::chip_stack_lock(portMAX_DELAY);
    attribute::report(s_ep, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
    if (ls == lock::SUCCESS) lock::chip_stack_unlock();
}

// Coalescing worker: always writes the CURRENT relay state, so rapid toggles
// collapse into one update. Also re-syncs every 30 s to self-heal any drift
// (e.g. a Matter write that the scheduler refused because the RTC was down).
static void sync_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        push_attribute(current_relay());
    }
}

void matter_sync_init(uint16_t endpoint_id) { s_ep = endpoint_id; }

void matter_sync_start(void)
{
    if (!s_task) xTaskCreate(sync_task, "matter_sync", 4096, nullptr, 4, &s_task);
    if (s_task) xTaskNotifyGive(s_task);          // initial sync of the real relay state
}

void matter_sync_notify_relay(bool)
{
    if (s_task) xTaskNotifyGive(s_task);
}
