#include "rtc_ds3231.h"
#include "app_priv.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "rtc";
static i2c_master_bus_handle_t s_bus = nullptr;
static i2c_master_dev_handle_t s_dev = nullptr;
static SemaphoreHandle_t s_mtx = nullptr;

static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

static esp_err_t bus_create(void)
{
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = I2C_NUM_0;
    cfg.sda_io_num = PIN_SDA;
    cfg.scl_io_num = PIN_SCL;
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;
    esp_err_t e = i2c_new_master_bus(&cfg, &s_bus);
    if (e != ESP_OK) return e;

    i2c_device_config_t d = {};
    d.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    d.device_address = RTC_I2C_ADDR;
    d.scl_speed_hz = 100000;
    e = i2c_master_bus_add_device(s_bus, &d, &s_dev);
    if (e != ESP_OK) { i2c_del_master_bus(s_bus); s_bus = nullptr; }
    return e;
}

static void bus_destroy(void)
{
    if (s_dev) { i2c_master_bus_rm_device(s_dev); s_dev = nullptr; }
    if (s_bus) { i2c_del_master_bus(s_bus); s_bus = nullptr; }
}

esp_err_t rtc_ds3231_init(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    return bus_create();
}

static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int dim(int y, int m)
{
    static const uint8_t d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && leap(y)) ? 29 : d[m - 1];
}

bool rtc_ds3231_valid(const rtc_time_t *t)
{
    if (t->year < MIN_VALID_YEAR || t->year > MAX_VALID_YEAR) return false;
    if (t->month < 1 || t->month > 12) return false;
    if (t->day < 1 || t->day > dim(t->year, t->month)) return false;
    return t->hour < 24 && t->minute < 60 && t->second < 60;
}

esp_err_t rtc_ds3231_read(rtc_time_t *out, const char **reason)
{
    uint8_t reg = 0x00, b[7] = {0}, sreg = 0x0F, st = 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t e = s_dev ? i2c_master_transmit_receive(s_dev, &reg, 1, b, 7, 100) : ESP_ERR_INVALID_STATE;
    if (e == ESP_OK) e = i2c_master_transmit_receive(s_dev, &sreg, 1, &st, 1, 100);
    xSemaphoreGive(s_mtx);

    if (e != ESP_OK) { *reason = "DS3231 I2C not responding"; return ESP_FAIL; }
    if (st & 0x80)   { *reason = "DS3231 lost power";         return ESP_ERR_INVALID_STATE; }

    rtc_time_t t;
    t.second = bcd2bin(b[0] & 0x7F);
    t.minute = bcd2bin(b[1] & 0x7F);
    if (b[2] & 0x40) {                       // 12-hour mode (we always write 24 h)
        uint8_t h = bcd2bin(b[2] & 0x1F) % 12;
        t.hour = (b[2] & 0x20) ? h + 12 : h;
    } else {
        t.hour = bcd2bin(b[2] & 0x3F);
    }
    t.day   = bcd2bin(b[4] & 0x3F);
    t.month = bcd2bin(b[5] & 0x1F);
    t.year  = 2000 + bcd2bin(b[6]);

    if (!rtc_ds3231_valid(&t)) { *reason = "Invalid RTC time"; return ESP_ERR_INVALID_RESPONSE; }
    *out = t;
    return ESP_OK;
}

esp_err_t rtc_ds3231_write(const rtc_time_t *t)
{
    uint8_t buf[8];
    buf[0] = 0x00;
    buf[1] = bin2bcd(t->second);
    buf[2] = bin2bcd(t->minute);
    buf[3] = bin2bcd(t->hour);                 // 24-hour mode
    buf[4] = 1;                                // weekday unused
    buf[5] = bin2bcd(t->day);
    buf[6] = bin2bcd(t->month);
    buf[7] = bin2bcd(t->year - 2000);

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_err_t e = s_dev ? i2c_master_transmit(s_dev, buf, sizeof(buf), 100) : ESP_ERR_INVALID_STATE;
    if (e == ESP_OK) {                         // clear OSF (bit 7 of status reg 0x0F)
        uint8_t sreg = 0x0F, st = 0;
        e = i2c_master_transmit_receive(s_dev, &sreg, 1, &st, 1, 100);
        if (e == ESP_OK && (st & 0x80)) {
            uint8_t w[2] = {0x0F, (uint8_t)(st & ~0x80)};
            e = i2c_master_transmit(s_dev, w, 2, 100);
        }
    }
    xSemaphoreGive(s_mtx);
    return e;
}

bool rtc_ds3231_recover_bus(void)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bus_destroy();

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << PIN_SDA) | (1ULL << PIN_SCL);
    io.mode = GPIO_MODE_INPUT_OUTPUT_OD;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    gpio_set_level(PIN_SDA, 1);
    gpio_set_level(PIN_SCL, 1);
    esp_rom_delay_us(10);

    for (int i = 0; i < 9 && gpio_get_level(PIN_SDA) == 0; i++) {
        gpio_set_level(PIN_SCL, 0); esp_rom_delay_us(5);
        gpio_set_level(PIN_SCL, 1); esp_rom_delay_us(5);
    }
    // STOP condition
    gpio_set_level(PIN_SDA, 0); esp_rom_delay_us(5);
    gpio_set_level(PIN_SCL, 1); esp_rom_delay_us(5);
    gpio_set_level(PIN_SDA, 1); esp_rom_delay_us(5);

    bool free_bus = gpio_get_level(PIN_SDA) && gpio_get_level(PIN_SCL);
    gpio_reset_pin(PIN_SDA);
    gpio_reset_pin(PIN_SCL);

    esp_err_t e = bus_create();
    xSemaphoreGive(s_mtx);
    ESP_LOGW(TAG, "I2C recovery: bus %s, driver %s", free_bus ? "free" : "STUCK", e == ESP_OK ? "ok" : "FAILED");
    return free_bus;
}
