#pragma once

// ============================================================
//  sensor_manager.h
//  Abstracts temp/RH and CO2 sensors behind a single interface.
//  Set SENSOR_TEMPRH_TYPE and SENSOR_CO2_TYPE in config.h to
//  select the physical sensor in use.
// ============================================================

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    float    temperature_c;     // °C (primary sensor)
    float    humidity_pct;      // % RH (primary sensor)
    float    humidity2_pct;     // % RH (secondary sensor, 0 if not fitted)
    uint16_t co2_ppm;           // ppm (0 = not available)
    bool     temp_rh_valid;     // primary sensor valid
    bool     temp_rh2_valid;    // secondary sensor valid
    bool     co2_valid;
    bool     level_low;         // true = water below XKC-Y25 sensor
    float    panel_temp_c;      // °C controller cabinet temperature
    bool     panel_temp_valid;  // true = panel DHT reading is valid
    time_t   last_updated;      // epoch seconds
} sensor_data_t;

// Returns the average humidity when both sensors are valid, otherwise
// the reading from whichever single sensor is valid (or 0 if neither).
float sensor_manager_effective_humidity(const sensor_data_t *d);

esp_err_t sensor_manager_init(void);

// Returns the shared I2C bus handle (valid after sensor_manager_init()).
// Used by relay.c to initialise the MCP23017 on the same bus.
i2c_master_bus_handle_t sensor_manager_get_i2c_bus(void);

// Read all sensors into the shared data structure.
// Called from the sensor task — do not call from multiple tasks simultaneously.
esp_err_t sensor_manager_read(void);

// Thread-safe copy of latest readings.
void sensor_manager_get(sensor_data_t *out);

// Returns true if the last successful read is within SENSOR_WATCHDOG_MS
bool sensor_manager_is_healthy(void);
