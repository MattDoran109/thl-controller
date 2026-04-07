// ============================================================
//  mcp23017.c — MCP23017 I²C I/O expander driver
//  Inline register-level I²C driver using esp-idf i2c_master.
//  BANK=0 (default, sequential register layout).
// ============================================================

#include "mcp23017.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "mcp23017";

// MCP23017 register addresses (BANK=0)
#define REG_IODIRA   0x00   // Port A direction (1=input, 0=output)
#define REG_IODIRB   0x01   // Port B direction
#define REG_GPPUA    0x0C   // Port A pull-up enable
#define REG_GPPUB    0x0D   // Port B pull-up enable
#define REG_GPIOA    0x12   // Port A GPIO read
#define REG_GPIOB    0x13   // Port B GPIO read
#define REG_OLATA    0x14   // Port A output latch
#define REG_OLATB    0x15   // Port B output latch

static i2c_master_dev_handle_t s_dev   = NULL;
static uint8_t                 s_olat[2] = {0xFF, 0xFF}; // shadow OLATA/OLATB

static esp_err_t mcp_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t mcp_read_reg(uint8_t reg, uint8_t *val)
{
    // Write register address, then read 1 byte (repeated start)
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

esp_err_t mcp23017_init(i2c_master_bus_handle_t bus)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = MCP23017_I2C_ADDR,
        .scl_speed_hz    = SENSOR_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add MCP23017: %s", esp_err_to_name(ret));
        return ret;
    }

    // Port A: all outputs
    ret = mcp_write_reg(REG_IODIRA, 0x00);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "IODIRA: %s", esp_err_to_name(ret)); return ret; }
    // Port B: GPB0 = input (door switch), GPB1-7 = outputs
    ret = mcp_write_reg(REG_IODIRB, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "IODIRB: %s", esp_err_to_name(ret)); return ret; }
    // Enable internal pull-up on GPB0 so an unconnected/open switch reads HIGH
    ret = mcp_write_reg(REG_GPPUB, 0x01);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "GPPUB: %s", esp_err_to_name(ret)); return ret; }

    // All outputs LOW → active-HIGH (ULN2003) loads start OFF
    ret = mcp_write_reg(REG_OLATA, 0x00);
    if (ret != ESP_OK) return ret;
    ret = mcp_write_reg(REG_OLATB, 0x00);
    if (ret != ESP_OK) return ret;

    s_olat[0] = 0x00;
    s_olat[1] = 0x00;

    ESP_LOGI(TAG, "MCP23017 ready at I2C addr 0x%02X", MCP23017_I2C_ADDR);
    return ESP_OK;
}

esp_err_t mcp23017_set_pin(uint8_t port, uint8_t pin, bool level)
{
    if (port > 1 || pin > 7) return ESP_ERR_INVALID_ARG;
    if (!s_dev)               return ESP_ERR_INVALID_STATE;

    if (level)
        s_olat[port] |=  (1u << pin);
    else
        s_olat[port] &= ~(1u << pin);

    uint8_t reg = (port == 0) ? REG_OLATA : REG_OLATB;
    return mcp_write_reg(reg, s_olat[port]);
}

esp_err_t mcp23017_read_pin(uint8_t port, uint8_t pin, bool *level)
{
    if (port > 1 || pin > 7 || !level) return ESP_ERR_INVALID_ARG;
    if (!s_dev)                         return ESP_ERR_INVALID_STATE;

    uint8_t val = 0;
    uint8_t reg = (port == 0) ? REG_GPIOA : REG_GPIOB;
    esp_err_t ret = mcp_read_reg(reg, &val);
    if (ret != ESP_OK) return ret;
    *level = (val >> pin) & 0x01;
    return ESP_OK;
}
