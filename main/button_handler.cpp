#include "button_handler.h"
#include "app_priv.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const int64_t DEBOUNCE_MS   = 40;
static const int64_t VERY_LONG_MS  = 10000;
static volatile uint16_t s_dbl = 300, s_long = 2000;
static button_cb_t s_cb;

static int64_t ms(void) { return esp_timer_get_time() / 1000; }

// Same state machine as v13, sampled every 5 ms. Polling (not ISR) is deliberate:
// debounce and multi-click windows need a time base anyway, and an ISR would
// only add wake-ups on a noisy contact.
static void button_task(void *)
{
    int  lastReading = gpio_get_level(PIN_BUTTON), stable = lastReading;
    bool pressed = false, longHandled = false, veryLongHandled = false;
    int64_t lastEdge = 0, pressStart = 0, pendingTime = 0;
    uint8_t clicks = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5));
        int reading = gpio_get_level(PIN_BUTTON);
        int64_t now = ms();

        if (reading != lastReading) lastEdge = now;

        if (now - lastEdge > DEBOUNCE_MS && reading != stable) {
            stable = reading;
            if (stable == 0) {                                   // pressed
                pressed = true; pressStart = now; veryLongHandled = false;
                if (clicks > 0) {
                    clicks++;
                    longHandled = true;                           // later presses never count as long
                    if (clicks >= 3) { clicks = 0; s_cb(BTN_TRIPLE); }
                } else longHandled = false;
            } else if (pressed) {                                 // released
                pressed = false;
                if (clicks > 0) pendingTime = now;
                else if (!longHandled) { clicks = 1; pendingTime = now; }
            }
        }

        if (clicks > 0 && reading == 1 && !pressed && now - pendingTime > s_dbl) {
            uint8_t n = clicks; clicks = 0;
            s_cb(n == 1 ? BTN_SINGLE : BTN_DOUBLE);
        }

        if (pressed && !longHandled && now - pressStart >= s_long) {
            longHandled = true; clicks = 0;
            s_cb(BTN_LONG);
        }
        if (pressed && !veryLongHandled && now - pressStart >= VERY_LONG_MS) {
            veryLongHandled = true;
            s_cb(BTN_VERY_LONG);
        }
        lastReading = reading;
    }
}

void button_handler_set_timing(uint16_t dbl_ms, uint16_t long_ms) { s_dbl = dbl_ms; s_long = long_ms; }

esp_err_t button_handler_start(button_cb_t cb, uint16_t dbl_ms, uint16_t long_ms)
{
    s_cb = cb;
    button_handler_set_timing(dbl_ms, long_ms);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_BUTTON;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    esp_err_t e = gpio_config(&io);
    if (e != ESP_OK) return e;
    return xTaskCreate(button_task, "button", 4096, nullptr, 6, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
