C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe -m esp_idf_monitor -p COM11 -b 115200 --no-reset --toolchain-prefix xtensa-esp32s3-elf- --target esp32s3 "c:\Users\doran\Documents\Al Wall Controller\build\al_wall_controller.elf"C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe -m esp_idf_monitor -p COM11 -b 115200 --no-reset --toolchain-prefix xtensa-esp32s3-elf- --target esp32s3 "c:\Users\doran\Documents\Al Wall Controller\build\al_wall_controller.elf"// ============================================================
//  sensor_manager.c
//  Inline lightweight I2C drivers for SHT31 and SCD41.
//  Swap SENSOR_TEMPRH_TYPE / SENSOR_CO2_TYPE in config.h.
// ============================================================

#include "sensor_manager.h"
#include "config.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char *TAG = "sensors";

// ---- Internal state ----------------------------------------
static i2c_master_bus_handle_t s_i2c_bus      = NULL;
static i2c_master_dev_handle_t s_temprh_dev   = NULL;
static i2c_master_dev_handle_t s_scd41_dev    = NULL;
static i2c_master_dev_handle_t s_lm75a_dev    = NULL;

static sensor_data_t   s_data        = {
    .temp_rh2_valid = false,
    .humidity2_pct  = 0.0f,
};
static SemaphoreHandle_t s_mutex     = NULL;
static bool            s_initialised = false;
static TickType_t      s_last_read_tick = 0;  // monotonic — immune to NTP jumps

// ============================================================
// Utility: I2C read/write helpers
// ============================================================
static esp_err_t i2c_write_cmd(i2c_master_dev_handle_t dev,
                                uint8_t *cmd, size_t len)
{
    return i2c_master_transmit(dev, cmd, len, pdMS_TO_TICKS(100));
}

static esp_err_t i2c_read(i2c_master_dev_handle_t dev,
                           uint8_t *buf, size_t len)
{
    return i2c_master_receive(dev, buf, len, pdMS_TO_TICKS(100));
}

// ============================================================
// SHT31 driver (temp + RH over I2C)
// ============================================================
#if SENSOR_TEMPRH_TYPE == SENSOR_TYPE_SHT31

static uint8_t sht31_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

static esp_err_t sht31_init(void)
{
    // Soft reset — retry up to 3 times in case sensor hasn't fully
    // powered up when init runs early in the boot sequence.
    uint8_t cmd[] = {0x30, 0xA2};
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = i2c_write_cmd(s_temprh_dev, cmd, sizeof(cmd));
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "SHT31 soft reset attempt %d failed: %s",
                 attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    return ret;
}

static esp_err_t sht31_read(float *temp_c, float *hum_pct)
{
    // Single-shot measurement, high repeatability
    uint8_t cmd[] = {0x24, 0x00};
    esp_err_t ret = i2c_write_cmd(s_temprh_dev, cmd, sizeof(cmd));
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t raw[6] = {0};
    ret = i2c_read(s_temprh_dev, raw, sizeof(raw));
    if (ret != ESP_OK) return ret;

    // Validate CRC
    if (sht31_crc(&raw[0], 2) != raw[2] ||
        sht31_crc(&raw[3], 2) != raw[5]) {
        ESP_LOGW(TAG, "SHT31 CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_t = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t raw_h = ((uint16_t)raw[3] << 8) | raw[4];

    *temp_c  = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    *hum_pct =          100.0f * ((float)raw_h / 65535.0f);
    return ESP_OK;
}

#elif SENSOR_TEMPRH_TYPE == SENSOR_TYPE_BME280

// ---- BME280 stub — fill in compensation data read + formulas
// or use the esp-idf-bme280 component from ESP-IDF component registry.
static esp_err_t sht31_init(void) {
    ESP_LOGW(TAG, "BME280 driver stub — implement or link component");
    return ESP_OK;
}
static esp_err_t sht31_read(float *temp_c, float *hum_pct) {
    (void)temp_c; (void)hum_pct;
    return ESP_ERR_NOT_SUPPORTED;
}

#elif SENSOR_TEMPRH_TYPE == SENSOR_TYPE_DHT22
// ----------------------------------------------------------------
// DHT22 (AM2302) single-wire bit-bang driver
// Timing-sensitive section runs inside a spinlock critical region
// to prevent task preemption during the ~5 ms read window.
// ----------------------------------------------------------------
static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

// Busy-wait for a GPIO level change. Returns µs waited, or -1 on timeout.
static inline int dht_await(int pin, int level, int timeout_us)
{
    int t = 0;
    while (gpio_get_level(pin) != level) {
        if (t >= timeout_us) return -1;
        esp_rom_delay_us(1);
        t++;
    }
    return t;
}

static esp_err_t sht31_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_DHT22,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(1000));   // DHT22 power-on stabilisation
    ESP_LOGI(TAG, "DHT22 ready on GPIO%d", PIN_DHT22);
    return ESP_OK;
}

static esp_err_t sht31_read(float *temp_c, float *hum_pct)
{
    uint8_t data[5] = {0};

    // Send start signal: drive low ≥1 ms (slow — OK outside critical section)
    gpio_set_direction(PIN_DHT22, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(PIN_DHT22, 0);
    vTaskDelay(pdMS_TO_TICKS(2));

    // Enter critical section BEFORE releasing the line.
    // The sensor responds within 20–40 µs of the release; if the scheduler
    // preempts between release and portENTER_CRITICAL we miss the window entirely.
    portENTER_CRITICAL(&s_dht_mux);
    gpio_set_level(PIN_DHT22, 1);
    gpio_set_direction(PIN_DHT22, GPIO_MODE_INPUT);
    esp_rom_delay_us(30);   // wait for sensor to start pulling low

    // --- Critical section: read 40 data bits (~5 ms) ---------------

    // DHT response: ~80 µs low, ~80 µs high
    if (dht_await(PIN_DHT22, 0, 100) < 0) goto timeout;
    if (dht_await(PIN_DHT22, 1, 100) < 0) goto timeout;
    // First bit preamble: ~50 µs low
    if (dht_await(PIN_DHT22, 0, 100) < 0) goto timeout;

    for (int i = 0; i < 40; i++) {
        // Wait for bit-high (end of 50 µs low preamble)
        if (dht_await(PIN_DHT22, 1, 100) < 0) goto timeout;
        // Sample at 40 µs: >40 µs high = '1', ≤40 µs = '0'
        esp_rom_delay_us(40);
        data[i / 8] <<= 1;
        if (gpio_get_level(PIN_DHT22) == 1) {
            data[i / 8] |= 1;
            // Wait for this bit's high to end before next preamble
            if (dht_await(PIN_DHT22, 0, 100) < 0) goto timeout;
        }
    }

    portEXIT_CRITICAL(&s_dht_mux);
    // ---------------------------------------------------------------

    // Checksum
    uint8_t sum = data[0] + data[1] + data[2] + data[3];
    if ((sum & 0xFF) != data[4]) {
        ESP_LOGW(TAG, "DHT22 checksum error: %02x != %02x", sum & 0xFF, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_h = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_t = ((uint16_t)(data[2] & 0x7F) << 8) | data[3];
    *hum_pct = raw_h / 10.0f;
    *temp_c  = raw_t / 10.0f;
    if (data[2] & 0x80) *temp_c = -(*temp_c);   // sign bit
    return ESP_OK;

timeout:
    portEXIT_CRITICAL(&s_dht_mux);
    ESP_LOGW(TAG, "DHT22 timeout");
    return ESP_ERR_TIMEOUT;
}

#elif SENSOR_TEMPRH_TYPE == SENSOR_TYPE_DHT11
// ----------------------------------------------------------------
// DHT11 single-wire driver
// Same protocol as DHT22 but:
//   - Start pulse must be >=18 ms (DHT11 ignores shorter pulses)
//   - Data is integer only: byte0=RH int, byte1=RH dec,
//     byte2=Temp int, byte3=Temp dec (no sign, no negatives)
// ----------------------------------------------------------------
static portMUX_TYPE s_dht_mux = portMUX_INITIALIZER_UNLOCKED;

static inline int dht_await(int pin, int level, int timeout_us)
{
    int t = 0;
    while (gpio_get_level(pin) != level) {
        if (t >= timeout_us) return -1;
        esp_rom_delay_us(1);
        t++;
    }
    return t;
}

static esp_err_t sht31_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_DHT22,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "DHT11 ready on GPIO%d", PIN_DHT22);
    return ESP_OK;
}

static esp_err_t sht31_read(float *temp_c, float *hum_pct)
{
    uint8_t data[5] = {0};

    // Start: drive low >=18 ms (DHT11 requirement)
    gpio_set_direction(PIN_DHT22, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(PIN_DHT22, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Enter critical section before releasing — sensor responds within ~40 µs
    portENTER_CRITICAL(&s_dht_mux);
    gpio_set_level(PIN_DHT22, 1);
    gpio_set_direction(PIN_DHT22, GPIO_MODE_INPUT);
    esp_rom_delay_us(30);

    // DHT response: ~80 µs low, ~80 µs high
    if (dht_await(PIN_DHT22, 0, 100) < 0) goto timeout;
    if (dht_await(PIN_DHT22, 1, 100) < 0) goto timeout;
    // First bit preamble: ~50 µs low
    if (dht_await(PIN_DHT22, 0, 100) < 0) goto timeout;

    for (int i = 0; i < 40; i++) {
        if (dht_await(PIN_DHT22, 1, 100) < 0) goto timeout;
        esp_rom_delay_us(40);
        data[i / 8] <<= 1;
        if (gpio_get_level(PIN_DHT22) == 1) {
            data[i / 8] |= 1;
            if (dht_await(PIN_DHT22, 0, 100) < 0) goto timeout;
        }
    }

    portEXIT_CRITICAL(&s_dht_mux);

    uint8_t sum = data[0] + data[1] + data[2] + data[3];
    if ((sum & 0xFF) != data[4]) {
        ESP_LOGW(TAG, "DHT11 checksum error: %02x != %02x", sum & 0xFF, data[4]);
        return ESP_ERR_INVALID_CRC;
    }

    // DHT11: byte0=RH integer, byte1=RH decimal, byte2=Temp int, byte3=Temp dec
    *hum_pct = data[0] + data[1] * 0.1f;
    *temp_c  = data[2] + data[3] * 0.1f;
    return ESP_OK;

timeout:
    portEXIT_CRITICAL(&s_dht_mux);
    ESP_LOGW(TAG, "DHT11 timeout");
    return ESP_ERR_TIMEOUT;
}
#endif  // SENSOR_TEMPRH_TYPE

// ============================================================
// SCD41 driver (CO2 + backup temp/RH, I2C)
// ============================================================
#if SENSOR_CO2_TYPE == SENSOR_TYPE_SCD41

static uint8_t scd41_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

static esp_err_t scd41_send_cmd(uint16_t cmd_word)
{
    uint8_t buf[2] = {cmd_word >> 8, cmd_word & 0xFF};
    return i2c_write_cmd(s_scd41_dev, buf, 2);
}

static esp_err_t scd41_init(void)
{
    // Stop any running measurement (covers warm/watchdog restart where
    // the sensor may be mid-measurement and holding the I2C bus).
    scd41_send_cmd(0x3F86);   // stop_periodic_measurement
    vTaskDelay(pdMS_TO_TICKS(500));
    // Reinitialise to factory state — essential after a warm restart.
    // perform_factory_reset (0x3632) is stronger than reinit (0x3646):
    // it resets all sensor registers, stopping any stuck measurement state
    // and releasing a held SDA line that would block the SHT31 too.
    esp_err_t fr = scd41_send_cmd(0x3632);   // perform_factory_reset
    ESP_LOGI(TAG, "SCD41 factory_reset: %s", esp_err_to_name(fr));
    vTaskDelay(pdMS_TO_TICKS(1200));          // factory_reset takes 1200 ms
    // Start periodic measurement
    esp_err_t ret = scd41_send_cmd(0x21B1);
    vTaskDelay(pdMS_TO_TICKS(5000)); // First reading takes ~5s
    ESP_LOGI(TAG, "SCD41 init done: %s", esp_err_to_name(ret));
    return ret;
}

static esp_err_t scd41_read(uint16_t *co2_ppm, float *temp_c, float *hum_pct)
{
    // Check data-ready status (cmd 0xE4B8) before reading.
    // SCD41 measures every 5s; if we poll between measurements the
    // chip may return stale or invalid data. Retry up to 5 times.
    bool data_ready = false;
    for (int i = 0; i < 5; i++) {
        esp_err_t dr_ret = scd41_send_cmd(0xE4B8);
        if (dr_ret != ESP_OK) return dr_ret;
        vTaskDelay(pdMS_TO_TICKS(1));
        uint8_t dr[3] = {0};
        dr_ret = i2c_read(s_scd41_dev, dr, sizeof(dr));
        if (dr_ret != ESP_OK) return dr_ret;
        if (scd41_crc(&dr[0], 2) != dr[2]) return ESP_ERR_INVALID_CRC;
        uint16_t status = ((uint16_t)dr[0] << 8) | dr[1];
        if (status & 0x07FF) { data_ready = true; break; }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!data_ready) {
        ESP_LOGW(TAG, "SCD41 data not ready — skipping read");
        return ESP_ERR_NOT_FOUND;
    }

    // Read measurement: 0xEC05
    esp_err_t ret = scd41_send_cmd(0xEC05);
    if (ret != ESP_OK) return ret;

    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t raw[9] = {0};
    ret = i2c_read(s_scd41_dev, raw, sizeof(raw));
    if (ret != ESP_OK) return ret;

    // Validate CRCs
    if (scd41_crc(&raw[0], 2) != raw[2] ||
        scd41_crc(&raw[3], 2) != raw[5] ||
        scd41_crc(&raw[6], 2) != raw[8]) {
        ESP_LOGW(TAG, "SCD41 CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    *co2_ppm = ((uint16_t)raw[0] << 8) | raw[1];
    uint16_t raw_t = ((uint16_t)raw[3] << 8) | raw[4];
    uint16_t raw_h = ((uint16_t)raw[6] << 8) | raw[7];

    *temp_c  = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    *hum_pct =          100.0f * ((float)raw_h / 65535.0f);
    return ESP_OK;
}

#elif SENSOR_CO2_TYPE == SENSOR_TYPE_MHZ19
// ----------------------------------------------------------------
// MH-Z19C CO2 sensor — 9600 8N1 UART
// Command frame:  0xFF 0x01 0x86 0x00*5 0x79
// Response frame: 0xFF 0x86 HIGH LOW  ... CHECKSUM  (9 bytes total)
// ----------------------------------------------------------------
static esp_err_t scd41_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = 9600,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_driver_install(MHZ19_UART_PORT, 256, 0, 0, NULL, 0);
    if (ret != ESP_OK) return ret;
    ret = uart_param_config(MHZ19_UART_PORT, &cfg);
    if (ret != ESP_OK) return ret;
    ret = uart_set_pin(MHZ19_UART_PORT,
                       PIN_MHZ19_TX, PIN_MHZ19_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;
    // MH-Z19C needs ~3 min to warm up for accurate readings;
    // we just start reading immediately and let the sensor self-stabilise.
    ESP_LOGI(TAG, "MH-Z19C UART ready on TX=%d RX=%d", PIN_MHZ19_TX, PIN_MHZ19_RX);
    return ESP_OK;
}

static esp_err_t scd41_read(uint16_t *co2_ppm, float *temp_c, float *hum_pct)
{
    // MH-Z19C does not provide RH; temp is internal and not accurate enough
    // to replace a dedicated sensor, so we leave those outputs unchanged.
    (void)temp_c;
    (void)hum_pct;

    // Flush any stale bytes in the RX buffer
    uart_flush_input(MHZ19_UART_PORT);

    // Send read-CO2 command
    const uint8_t cmd[9] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
    uart_write_bytes(MHZ19_UART_PORT, (const char *)cmd, sizeof(cmd));

    // Wait for 9-byte response (allow up to 100 ms)
    uint8_t resp[9] = {0};
    int got = uart_read_bytes(MHZ19_UART_PORT, resp, sizeof(resp),
                              pdMS_TO_TICKS(100));
    if (got != 9) {
        ESP_LOGW(TAG, "MH-Z19C: short read (%d bytes)", got);
        return ESP_ERR_TIMEOUT;
    }

    // Validate header byte and checksum
    if (resp[0] != 0xFF || resp[1] != 0x86) {
        ESP_LOGW(TAG, "MH-Z19C: bad header %02x %02x", resp[0], resp[1]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t csum = 0;
    for (int i = 1; i <= 7; i++) csum += resp[i];
    csum = (~csum) + 1;
    if (csum != resp[8]) {
        ESP_LOGW(TAG, "MH-Z19C: checksum error");
        return ESP_ERR_INVALID_CRC;
    }

    *co2_ppm = ((uint16_t)resp[2] << 8) | resp[3];
    ESP_LOGI(TAG, "MH-Z19C CO2: %u ppm", *co2_ppm);
    return ESP_OK;
}

#endif  // SENSOR_CO2_TYPE

// ============================================================
// Panel DHT22 — controller cabinet temperature sensor
// Compiled whenever PIN_PANEL_DHT is defined in config.h.
// Compatible with both DHT11 and DHT22 (uses >=18 ms start pulse).
// ============================================================
#ifdef PIN_PANEL_DHT
static portMUX_TYPE s_panel_dht_mux = portMUX_INITIALIZER_UNLOCKED;

static inline int panel_dht_await(int pin, int level, int timeout_us)
{
    int t = 0;
    while (gpio_get_level(pin) != level) {
        if (t >= timeout_us) return -1;
        esp_rom_delay_us(1);
        t++;
    }
    return t;
}

static void panel_dht_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_PANEL_DHT,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Panel DHT ready on GPIO%d", PIN_PANEL_DHT);
}

static esp_err_t panel_dht_read(float *temp_c, float *hum_pct)
{
    uint8_t data[5] = {0};

    // Start: drive LOW 20 ms (satisfies both DHT11 >=18 ms and DHT22 >=1 ms)
    gpio_set_direction(PIN_PANEL_DHT, GPIO_MODE_OUTPUT_OD);
    gpio_set_level(PIN_PANEL_DHT, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    portENTER_CRITICAL(&s_panel_dht_mux);
    gpio_set_level(PIN_PANEL_DHT, 1);
    gpio_set_direction(PIN_PANEL_DHT, GPIO_MODE_INPUT);
    esp_rom_delay_us(30);

    if (panel_dht_await(PIN_PANEL_DHT, 0, 100) < 0) goto timeout;
    if (panel_dht_await(PIN_PANEL_DHT, 1, 100) < 0) goto timeout;
    if (panel_dht_await(PIN_PANEL_DHT, 0, 100) < 0) goto timeout;

    for (int i = 0; i < 40; i++) {
        if (panel_dht_await(PIN_PANEL_DHT, 1, 100) < 0) goto timeout;
        esp_rom_delay_us(40);
        data[i / 8] <<= 1;
        if (gpio_get_level(PIN_PANEL_DHT) == 1) {
            data[i / 8] |= 1;
            if (panel_dht_await(PIN_PANEL_DHT, 0, 100) < 0) goto timeout;
        }
    }
    portEXIT_CRITICAL(&s_panel_dht_mux);

    if (((data[0] + data[1] + data[2] + data[3]) & 0xFF) != data[4]) {
        ESP_LOGW(TAG, "Panel DHT checksum error");
        return ESP_ERR_INVALID_CRC;
    }

#if PANEL_SENSOR_TYPE == SENSOR_TYPE_DHT11
    // DHT11: each byte is an integer value; byte0=RH int, byte2=Temp int
    *hum_pct = (float)data[0];
    *temp_c  = (float)data[2];
#else
    // DHT22: 16-bit values, /10.0 for one decimal place
    *hum_pct = (((uint16_t)data[0] << 8) | data[1]) / 10.0f;
    uint16_t raw_t = ((uint16_t)(data[2] & 0x7F) << 8) | data[3];
    *temp_c = raw_t / 10.0f;
    if (data[2] & 0x80) *temp_c = -(*temp_c);
#endif
    return ESP_OK;

timeout:
    portEXIT_CRITICAL(&s_panel_dht_mux);
    ESP_LOGW(TAG, "Panel DHT timeout");
    return ESP_ERR_TIMEOUT;
}
#endif  // PIN_PANEL_DHT

// ============================================================
// LM75A driver — panel (cabinet) temperature sensor
// 11-bit two's complement, 0.125 °C/LSB, register 0x00.
// ============================================================
#ifdef LM75A_I2C_ADDR

static esp_err_t lm75a_init(void)
{
    // Confirm comms by writing the pointer register — no other init needed.
    uint8_t ptr = 0x00;
    esp_err_t ret = i2c_master_transmit(s_lm75a_dev, &ptr, 1, pdMS_TO_TICKS(100));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "LM75A ready at 0x%02x", LM75A_I2C_ADDR);
    } else {
        ESP_LOGW(TAG, "LM75A not found at 0x%02x: %s", LM75A_I2C_ADDR, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t lm75a_read(float *temp_c)
{
    // Select temperature register then read 2 bytes MSB-first.
    // Bits[15:5] = 11-bit two's complement value, 0.125 °C per LSB.
    uint8_t ptr  = 0x00;
    uint8_t raw[2] = {0};
    esp_err_t ret = i2c_master_transmit_receive(s_lm75a_dev,
                                                &ptr, 1,
                                                raw, 2,
                                                pdMS_TO_TICKS(100));
    if (ret != ESP_OK) return ret;
    int16_t val = (int16_t)(((uint16_t)raw[0] << 8) | raw[1]);
    val >>= 5;   // shift to 11-bit two's complement
    *temp_c = val * 0.125f;
    return ESP_OK;
}

#endif  // LM75A_I2C_ADDR

// ============================================================
// Public API
// ============================================================
esp_err_t sensor_manager_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // I2C bus is only needed for I2C-based sensors (SHT31, BME280, SCD41).
    // Skip init entirely when only GPIO sensors (DHT11/DHT22) are used.
#if SENSOR_TEMPRH_TYPE == SENSOR_TYPE_SHT31 || \
    SENSOR_TEMPRH_TYPE == SENSOR_TYPE_BME280 || \
    SENSOR_CO2_TYPE    == SENSOR_TYPE_SCD41

    // ── I2C bus recovery ──────────────────────────────────────────
    // If a reset interrupted a transfer mid-byte, a slave may be holding
    // SDA LOW, which prevents i2c_new_master_bus from working.
    // Standard fix: bit-bang SCL 9 times with SDA high to clock the slave
    // out of its state, then send a STOP condition.
    {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << PIN_SENSOR_SDA) | (1ULL << PIN_SENSOR_SCL),
            .mode         = GPIO_MODE_OUTPUT_OD,   // open-drain so slaves can pull
            .pull_up_en   = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(PIN_SENSOR_SDA, 1);
        for (int i = 0; i < 9; i++) {
            gpio_set_level(PIN_SENSOR_SCL, 0);
            esp_rom_delay_us(5);
            gpio_set_level(PIN_SENSOR_SCL, 1);
            esp_rom_delay_us(5);
        }
        // STOP condition: SDA low → high while SCL is high
        gpio_set_level(PIN_SENSOR_SDA, 0);
        esp_rom_delay_us(5);
        gpio_set_level(PIN_SENSOR_SCL, 1);
        esp_rom_delay_us(5);
        gpio_set_level(PIN_SENSOR_SDA, 1);
        esp_rom_delay_us(5);
        // Release pins back to input so the I2C peripheral can take over
        gpio_reset_pin(PIN_SENSOR_SDA);
        gpio_reset_pin(PIN_SENSOR_SCL);
        ESP_LOGI(TAG, "I2C bus recovery pulse done");
        vTaskDelay(pdMS_TO_TICKS(50));  // let slaves settle before bus init
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = SENSOR_I2C_PORT,
        .sda_io_num    = PIN_SENSOR_SDA,
        .scl_io_num    = PIN_SENSOR_SCL,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    // Probe the expected I2C addresses — results appear in the serial log and
    // help identify address conflicts, missing pull-ups, or wiring mistakes.
    {
        static const struct { uint8_t a; const char *n; } k[] = {
            {0x20, "MCP23017   "}, {0x44, "SHT31@0x44 "}, {0x45, "SHT31@0x45 "},
            {0x48, "LM75A      "}, {0x62, "SCD41      "},
        };
        ESP_LOGI(TAG, "I2C probe (SDA=GPIO%d SCL=GPIO%d):", PIN_SENSOR_SDA, PIN_SENSOR_SCL);
        for (int i = 0; i < 5; i++) {
            esp_err_t pr = i2c_master_probe(s_i2c_bus, k[i].a, 20);
            ESP_LOGI(TAG, "  0x%02x %s %s", k[i].a, k[i].n, pr == ESP_OK ? "FOUND" : "---");
        }
    }
#endif

    // Add temp/RH device (SHT31 or BME280)
#if SENSOR_TEMPRH_TYPE == SENSOR_TYPE_SHT31 || \
    SENSOR_TEMPRH_TYPE == SENSOR_TYPE_BME280
    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = (SENSOR_TEMPRH_TYPE == SENSOR_TYPE_SHT31)
                               ? SHT31_I2C_ADDR : BME280_I2C_ADDR,
            .scl_speed_hz    = SENSOR_I2C_FREQ_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg,
                                                   &s_temprh_dev));
    }
#endif

#if SENSOR_TEMPRH_TYPE == SENSOR_TYPE_SHT31
    // If SHT31 is not at the configured address (0x44), try the alternate (0x45).
    // This handles boards where the ADDR pin is wired to VDD instead of GND.
    if (i2c_master_probe(s_i2c_bus, SHT31_I2C_ADDR, 20) != ESP_OK) {
        uint8_t alt = SHT31_I2C_ADDR ^ 0x01;   // 0x44 ↔ 0x45
        if (i2c_master_probe(s_i2c_bus, alt, 20) == ESP_OK) {
            ESP_LOGW(TAG, "SHT31 not at 0x%02x — found at 0x%02x, switching",
                     SHT31_I2C_ADDR, alt);
            i2c_master_bus_rm_device(s_temprh_dev);
            i2c_device_config_t alt_cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address  = alt,
                .scl_speed_hz    = SENSOR_I2C_FREQ_HZ,
            };
            ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &alt_cfg, &s_temprh_dev));
        } else {
            ESP_LOGW(TAG, "SHT31 not found at 0x44 or 0x45 — check wiring");
        }
    }
#endif

    // Add CO2 device (SCD41)
#if SENSOR_CO2_TYPE == SENSOR_TYPE_SCD41
    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = SCD41_I2C_ADDR,
            .scl_speed_hz    = SENSOR_I2C_FREQ_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg,
                                                   &s_scd41_dev));
    }
#endif

    // Add LM75A panel temperature sensor
#ifdef LM75A_I2C_ADDR
    {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = LM75A_I2C_ADDR,
            .scl_speed_hz    = SENSOR_I2C_FREQ_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg,
                                                   &s_lm75a_dev));
    }
#endif

    // Initialise physical sensors
    esp_err_t ret = sht31_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Temp/RH sensor init failed: %s",
                                esp_err_to_name(ret));

    ret = scd41_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "CO2 sensor init failed: %s",
                                esp_err_to_name(ret));

#ifdef LM75A_I2C_ADDR
    ret = lm75a_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "LM75A init failed: %s",
                                esp_err_to_name(ret));
#endif

    s_initialised = true;
    ESP_LOGI(TAG, "Sensor manager initialised");

    // Level sensor — simple GPIO input with pull-up
    gpio_config_t lv_cfg = {
        .pin_bit_mask = 1ULL << PIN_LEVEL_SENSOR,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&lv_cfg);
    ESP_LOGI(TAG, "Level sensor ready on GPIO%d", PIN_LEVEL_SENSOR);

#ifdef PIN_PANEL_DHT
    panel_dht_gpio_init();
#endif

    return ESP_OK;
}

esp_err_t sensor_manager_read(void)
{
    if (!s_initialised) return ESP_ERR_INVALID_STATE;

    float temp = 0, hum = 0;
    uint16_t co2 = 0;
    float co2_temp = 0, co2_hum = 0;

#ifdef LM75A_I2C_ADDR
    float panel_temp = 0;
    bool panel_ok = (lm75a_read(&panel_temp) == ESP_OK);
    if (panel_ok) ESP_LOGI(TAG, "Panel: %.1f°C", panel_temp);
#endif

    bool tr_ok = (sht31_read(&temp, &hum) == ESP_OK);
    if (!tr_ok) ESP_LOGW(TAG, "SHT31 read failed");

#if SENSOR_CO2_TYPE == SENSOR_TYPE_SCD41
    bool co2_ok = (scd41_read(&co2, &co2_temp, &co2_hum) == ESP_OK);
    if (!co2_ok) ESP_LOGW(TAG, "SCD41 read failed");
#else
    bool co2_ok = false;
#endif

    // Use SCD41 temp/RH as fallback if primary sensor failed
    if (!tr_ok && co2_ok) {
        temp   = co2_temp;
        hum    = co2_hum;
        tr_ok  = true;
    }

    if (tr_ok) {
        ESP_LOGI(TAG, "Temp: %.1f°C  RH: %.1f%%", temp, hum);
    }
    if (co2_ok) {
        ESP_LOGI(TAG, "CO2: %u ppm", co2);
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (tr_ok) {
        s_data.temperature_c = temp;
        s_data.humidity_pct  = hum;
        s_data.temp_rh_valid = true;
    }
    if (co2_ok) {
        s_data.co2_ppm        = co2;
        s_data.co2_valid      = true;
        s_data.humidity2_pct  = co2_hum;   // SCD41 secondary RH for averaging
        s_data.temp_rh2_valid = true;
    }
    // Level sensor — read every cycle regardless of other sensor health
    s_data.level_low = (gpio_get_level(PIN_LEVEL_SENSOR) == LEVEL_SENSOR_LOW);
#ifdef LM75A_I2C_ADDR
    if (panel_ok) {
        s_data.panel_temp_c     = panel_temp;
        s_data.panel_temp_valid = true;
    }
#endif
    if (tr_ok || co2_ok) {
        s_data.last_updated = time(NULL);
        s_last_read_tick    = xTaskGetTickCount();
    }
    xSemaphoreGive(s_mutex);

    return (tr_ok || co2_ok) ? ESP_OK : ESP_FAIL;
}

i2c_master_bus_handle_t sensor_manager_get_i2c_bus(void)
{
    return s_i2c_bus;
}

void sensor_manager_get(sensor_data_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_mutex);
}

bool sensor_manager_is_healthy(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    TickType_t last = s_last_read_tick;
    xSemaphoreGive(s_mutex);

    // Never had a reading yet — don't fault, hardware may not be connected.
    if (last == 0) return true;

    // Use monotonic tick count so NTP wall-clock jumps can't trigger a
    // spurious fault (the old time(NULL) approach broke on every reboot
    // once NTP had synced).
    TickType_t age_ms = (xTaskGetTickCount() - last) * portTICK_PERIOD_MS;
    return age_ms < SENSOR_WATCHDOG_MS;
}

float sensor_manager_effective_humidity(const sensor_data_t *d)
{
    if (d->temp_rh_valid && d->temp_rh2_valid)
        return (d->humidity_pct + d->humidity2_pct) * 0.5f;
    if (d->temp_rh2_valid) return d->humidity2_pct;
    return d->humidity_pct;  // primary only, or 0 if neither valid
}
