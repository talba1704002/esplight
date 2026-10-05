// Wi-Fi "LIGHT" access point + persisted advanced settings.
// The router (station) side belongs to Matter; this module only adds the AP,
// so the Wi-Fi mode becomes APSTA while the AP is on.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint8_t  magic;
    uint8_t  tx_idx;       // 0..10 into the TX table (2 dBm .. 19.5 dBm)
    uint8_t  channel;      // 1..13 (follows the router channel while STA is connected)
    uint8_t  proto;        // 0 = b, 1 = b/g, 2 = b/g/n
    uint8_t  max_clients;  // 1..4
    uint8_t  auto_off;     // 1 = AP off after 2 min without a phone
    uint16_t cpu_mhz;      // 160 or 240
    uint16_t dbl_ms;       // double-click window 200..600
    uint16_t long_ms;      // long press 1000..5000
    char     ssid[33];
    char     pass[64];     // empty = open network
} adv_settings_t;

#define TX_LEVEL_COUNT 11

esp_err_t ap_manager_init(void);        // loads NVS (call before button_handler_start)
esp_err_t ap_manager_start(void);       // after esp_matter::start(): starts the AP task
void      ap_manager_get(adv_settings_t *out);
// ESP_OK / ESP_ERR_INVALID_ARG (msg set) / ESP_FAIL (applied, NVS write failed).
esp_err_t ap_manager_set(const adv_settings_t *in, const char **msg);
esp_err_t ap_manager_reset_all(const char **msg);   // /api/adv {reset=1}
// Deferred requests, executed by the AP task (safe from any task / HTTP handler).
void      ap_manager_request_toggle(void);          // double click
void      ap_manager_request_ap_reset(void);        // triple click
bool      ap_manager_is_on(void);
