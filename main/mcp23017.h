#pragma once

// ============================================================
//  mcp23017.h — MCP23017 16-bit I/O expander driver
//  Uses ESP-IDF i2c_master (new API, IDF >= 5.x)
//
//  All 16 GPIO pins are configured as outputs at init.
//  Port A (GPA0-7) → relay board.  Port B (GPB0-7) → opto board.
//  All outputs initialise HIGH (active-LOW loads start OFF).
// ============================================================

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <stdbool.h>

// Initialise driver and register device on the shared I2C bus.
// Must be called after the bus has been created (i.e. after
// sensor_manager_init()).
esp_err_t mcp23017_init(i2c_master_bus_handle_t bus);

// Set a single output pin.
// port: 0 = GPA, 1 = GPB
// pin:  0-7
// level: true = HIGH, false = LOW
esp_err_t mcp23017_set_pin(uint8_t port, uint8_t pin, bool level);

// Read a single pin (for input-configured pins).
// port: 0 = GPA, 1 = GPB   pin: 0-7
// Returns the current GPIO level in *level (true = HIGH).
esp_err_t mcp23017_read_pin(uint8_t port, uint8_t pin, bool *level);
