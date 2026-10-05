// DS3231 driver on the ESP-IDF v5.2+ I2C master API (thread-safe).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} rtc_time_t;

esp_err_t rtc_ds3231_init(void);
bool      rtc_ds3231_valid(const rtc_time_t *t);
// ESP_OK, or an error with *reason set (I2C dead / oscillator stopped / bad time).
esp_err_t rtc_ds3231_read(rtc_time_t *out, const char **reason);
// Writes the time and clears the oscillator-stop flag.
esp_err_t rtc_ds3231_write(const rtc_time_t *t);
// Frees a stuck bus (9 SCL pulses + STOP) and recreates the driver.
// Returns true if SDA and SCL were both high afterwards.
bool      rtc_ds3231_recover_bus(void);
