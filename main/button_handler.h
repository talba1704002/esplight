// Non-blocking multi-click detector (single / double / triple / long / very long).
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    BTN_SINGLE,       // toggle relay (FORCE_ON/FORCE_OFF for the current period)
    BTN_DOUBLE,       // Wi-Fi AP on/off
    BTN_TRIPLE,       // reset AP / advanced Wi-Fi settings to defaults
    BTN_LONG,         // back to AUTO (hold long_ms, default 2 s)
    BTN_VERY_LONG     // factory-reset the Matter fabric (hold 10 s)
} button_event_t;

// Callback runs in the button task: keep it short (post a request, don't block).
typedef void (*button_cb_t)(button_event_t ev);

esp_err_t button_handler_start(button_cb_t cb, uint16_t dbl_ms, uint16_t long_ms);
void      button_handler_set_timing(uint16_t dbl_ms, uint16_t long_ms);   // from the web UI
