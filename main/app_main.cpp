// ESP32 + DS3231 light controller as a Matter 1.2 On/Off Light (Google Home).
//
// Task map
//   Matter/CHIP task   : owned by the SDK; runs attribute callbacks below
//   "sched"            : DS3231 polling, schedule evaluation, relay
//   "button"           : multi-click detector
//   "matter_sync"      : writes relay changes into the OnOff attribute
//   "ap_mgr"           : "LIGHT" access point
//   httpd              : web portal / JSON API
#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_matter.h>
#include <esp_matter_endpoint.h>
#include <app/server/Server.h>
#include <platform/CHIPDeviceLayer.h>

#include "app_priv.h"
#include "relay_scheduler.h"
#include "matter_sync.h"
#include "web_server.h"
#include "button_handler.h"
#include "ap_manager.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static uint16_t s_light_ep = 0;

// ---------------- Matter -> local ----------------
// PRE_UPDATE runs in the Matter task BEFORE the attribute is written. Returning an
// error makes the controller (Google Home) see a failed command.
static esp_err_t app_attribute_update_cb(callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    if (type != PRE_UPDATE || endpoint_id != s_light_ep ||
        cluster_id != OnOff::Id || attribute_id != OnOff::Attributes::OnOff::Id)
        return ESP_OK;

    if (matter_sync_is_internal_update()) return ESP_OK;     // our own mirror write, nothing to apply

    const char *msg = "";
    bool ok = relay_scheduler_set_override(val->val.b ? OVR_FORCE_ON : OVR_FORCE_OFF, SRC_MATTER, &msg);
    ESP_LOGI(TAG, "Matter %s -> %s (%s)", val->val.b ? "ON" : "OFF", ok ? "applied" : "REFUSED", msg);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t app_identification_cb(identification::callback_type_t, uint16_t, uint8_t, uint8_t, void *)
{
    return ESP_OK;                                           // no identify LED on this board
}

// ---------------- local -> Matter ----------------
// Called by the scheduler for every relay change (schedule, button, web, Matter),
// outside its lock. Just wakes the sync task.
static void on_relay_changed(bool on, change_src_t src)
{
    ESP_LOGI(TAG, "Relay %s (source %d)", on ? "ON" : "OFF", (int)src);
    matter_sync_notify_relay(on);
}

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    using namespace chip::DeviceLayer;
    switch (event->Type) {
    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        matter_sync_force_report();
        break;
    case DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {   // allow re-pairing
            auto &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            if (!mgr.IsCommissioningWindowOpen())
                mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(300));
        }
        break;
    }
    case DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "IP address changed");
        break;
    default:
        break;
    }
}

// ---------------- button ----------------
static void on_button(button_event_t ev)
{
    const char *msg = "";
    switch (ev) {
    case BTN_SINGLE:    relay_scheduler_toggle(SRC_BUTTON, &msg); break;
    case BTN_LONG:      relay_scheduler_set_override(OVR_AUTO, SRC_BUTTON, &msg); break;
    case BTN_DOUBLE:    ap_manager_request_toggle(); break;
    case BTN_TRIPLE:    ap_manager_request_ap_reset(); break;
    case BTN_VERY_LONG: ESP_LOGW(TAG, "10 s hold -> Matter factory reset"); esp_matter::factory_reset(); break;
    }
}

extern "C" void app_main()
{
    relay_scheduler_early_init();                            // relay OFF before anything else

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(ap_manager_init());                      // advanced settings from NVS
    adv_settings_t adv; ap_manager_get(&adv);

    relay_scheduler_set_change_cb(on_relay_changed);
    ESP_ERROR_CHECK(relay_scheduler_start());                // schedule, RTC, scheduler task
    ESP_ERROR_CHECK(button_handler_start(on_button, adv.dbl_ms, adv.long_ms));

    // ---- Matter node + On/Off Light endpoint ----
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (!node) { ESP_LOGE(TAG, "Matter node creation failed"); return; }

    on_off_light::config_t light_config;
    light_config.on_off.on_off = false;
    light_config.on_off.lighting.start_up_on_off = nullptr;  // never let Matter re-apply a stale state at boot
    endpoint_t *ep = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!ep) { ESP_LOGE(TAG, "Light endpoint creation failed"); return; }
    s_light_ep = endpoint::get_id(ep);
    matter_sync_init(s_light_ep);
    ESP_LOGI(TAG, "On/Off Light endpoint id %u", s_light_ep);

    err = esp_matter::start(app_event_cb);                   // BLE + Wi-Fi + mDNS + CHIP task
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(err)); return; }

    matter_sync_start();                                     // mirror the real relay state into the cluster
    ESP_ERROR_CHECK(ap_manager_start());
    web_server_start();                                      // reachable on the AP and on the router IP
}
