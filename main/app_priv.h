// Board / policy configuration for the Matter light controller.
#pragma once
#include "driver/gpio.h"

// ---- Hardware ----
#define PIN_RELAY          GPIO_NUM_18
#define PIN_BUTTON         GPIO_NUM_19   // to GND, internal pull-up
#define PIN_SDA            GPIO_NUM_21
#define PIN_SCL            GPIO_NUM_22
#define RTC_I2C_ADDR       0x68
#define RELAY_ACTIVE_HIGH  0            // 0 = active-LOW relay module (as v13)

// ---- Scheduler ----
#define MAX_SCHEDULES      10
#define MIN_VALID_YEAR     2024
#define MAX_VALID_YEAR     2099
#define DEFAULT_ON_H       6
#define DEFAULT_ON_M       0
#define DEFAULT_OFF_H      0
#define DEFAULT_OFF_M      0

// ---- Storage ----
#define NVS_NS             "light_ctrl"

// ---- Wi-Fi AP ("LIGHT") ----
#define AP_DEFAULT_SSID    "LIGHT"
#define AP_DEFAULT_ON      1
// AP IP is the ESP-IDF default 192.168.4.1. v13 used 192.168.1.1, which clashes
// with most home routers now that the ESP is also a station on that router.
