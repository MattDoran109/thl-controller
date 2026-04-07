// ============================================================
//  relay.c — Relay output management via MCP23017 I/O expander
//  All four relay channels route through the MCP23017 (I2C).
//  RELAY_HEATER additionally drives GPIO10 (SSR direct output).
//  Active level set by RELAY_ACTIVE_LEVEL in config.h (0 = LOW).
// ============================================================

#include "relay.h"
#include "config.h"
#include "mcp23017.h"
#include "sensor_manager.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "relay";

// MCP23017 port/pin for each relay channel
typedef struct { uint8_t port; uint8_t pin; } mcp_pin_t;

// port=0xFF means no MCP23017 pin (SSR-only relay)
static const mcp_pin_t RELAY_MCP[RELAY_COUNT] = {
    [RELAY_HEATER]     = {0xFF, 0},                       // SSR only
    [RELAY_HUMIDIFIER] = {0,   MCP23017_GPA_HUMIDIFIER},  // GPA0
    [RELAY_FAN]        = {0,   MCP23017_GPA_FAN},          // GPA2
    [RELAY_PANEL_FAN]  = {0,   MCP23017_GPA_PANEL_FAN},   // GPA1
    [RELAY_LIGHT]      = {0,   MCP23017_GPA_LIGHT},        // GPA3
};

static const char *RELAY_NAMES[RELAY_COUNT] = {
    [RELAY_HEATER]     = "Heater",
    [RELAY_HUMIDIFIER] = "Humidifier",
    [RELAY_FAN]        = "Fan",
    [RELAY_PANEL_FAN]  = "PanelFan",
    [RELAY_LIGHT]      = "Light",
};

static relay_status_t s_status[RELAY_COUNT];

// Logical ON/OFF → physical pin level
// RELAY_ACTIVE_LEVEL=0: ON→LOW, OFF→HIGH
static inline bool _pin_level(bool on)
{
    return on ? (bool)RELAY_ACTIVE_LEVEL : !(bool)RELAY_ACTIVE_LEVEL;
}

esp_err_t relay_init(void)
{
    // I2C bus must already exist (sensor_manager_init() called first)
    i2c_master_bus_handle_t bus = sensor_manager_get_i2c_bus();
    esp_err_t ret = mcp23017_init(bus);
    if (ret != ESP_OK) return ret;

    // GPIO10 → SSR heater direct output (active-HIGH: HIGH = heater ON)
    gpio_config_t ssr_cfg = {
        .pin_bit_mask = 1ULL << PIN_SSR_HEATER,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&ssr_cfg);
    gpio_set_level(PIN_SSR_HEATER, 0);   // SSR off at startup

    // Initialise state tracking — MCP23017 already set all outputs HIGH (OFF)
    for (int i = 0; i < RELAY_COUNT; i++) {
        s_status[i].state      = false;
        s_status[i].locked_off = false;
        strncpy(s_status[i].name, RELAY_NAMES[i], sizeof(s_status[i].name) - 1);
    }

    ESP_LOGI(TAG, "Relay outputs initialised via MCP23017 (active-%s)",
             RELAY_ACTIVE_LEVEL ? "HIGH" : "LOW");
    return ESP_OK;
}

esp_err_t relay_set(relay_id_t relay, bool on)
{
    if (relay >= RELAY_COUNT) return ESP_ERR_INVALID_ARG;

    if (on && s_status[relay].locked_off) {
        ESP_LOGW(TAG, "%s is locked OFF — ignoring set ON request",
                 RELAY_NAMES[relay]);
        return ESP_ERR_INVALID_STATE;
    }

    s_status[relay].state = on;

    // Drive MCP23017 output (skip if this relay has no MCP23017 pin)
    esp_err_t ret = ESP_OK;
    if (RELAY_MCP[relay].port != 0xFF) {
        ret = mcp23017_set_pin(RELAY_MCP[relay].port,
                               RELAY_MCP[relay].pin,
                               _pin_level(on));
    }

    // Mirror heater to GPIO10 SSR (active-HIGH)
    if (relay == RELAY_HEATER) {
        gpio_set_level(PIN_SSR_HEATER, on ? 1 : 0);
    }

    ESP_LOGI(TAG, "%s -> %s", RELAY_NAMES[relay], on ? "ON" : "OFF");
    return ret;
}

bool relay_get(relay_id_t relay)
{
    if (relay >= RELAY_COUNT) return false;
    return s_status[relay].state;
}

void relay_lock_off(relay_id_t relay, bool lock)
{
    if (relay >= RELAY_COUNT) return;
    s_status[relay].locked_off = lock;
    if (lock && s_status[relay].state) {
        relay_set(relay, false);  // Immediately turn off if locked
        ESP_LOGW(TAG, "%s locked OFF and turned off", RELAY_NAMES[relay]);
    }
}

bool relay_is_locked(relay_id_t relay)
{
    if (relay >= RELAY_COUNT) return false;
    return s_status[relay].locked_off;
}

void relay_all_off(void)
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        relay_set((relay_id_t)i, false);
    }
    ESP_LOGW(TAG, "All relays forced OFF");
}

void relay_get_all_status(relay_status_t out[RELAY_COUNT])
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        out[i] = s_status[i];
    }
}
