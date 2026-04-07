#pragma once

// ============================================================
//  controller.h — Environmental control logic
//  Hysteresis-based on/off control for all outputs.
//  Setpoints are persisted in NVS.
// ============================================================

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "relay.h"

typedef struct {
    float    temp_setpoint;       // °C
    float    temp_hysteresis;     // °C
    float    hum_setpoint;        // % RH
    float    hum_hysteresis;      // % RH
    uint16_t co2_threshold;       // ppm
    uint16_t co2_hysteresis;      // ppm
    int      fan_sched_on_min;    // minutes fan runs per period (0 = disabled)
    int      fan_sched_period_min;// period length in minutes
    bool     fan_sched_enabled;   // timed ventilation on/off
    int      light_on_hour;       // 0-23
    int      light_on_min;        // 0-59
    int      light_off_hour;      // 0-23
    int      light_off_min;       // 0-59
    bool     light_schedule_enabled;
    int      light_sunrise_min;   // 0 = instant on,  >0 = ramp over N minutes
    int      light_sunset_min;    // 0 = instant off, >0 = ramp over N minutes
    uint8_t  light_colour_temp;   // 0=Cool/100kHz  1=Neutral/65kHz  2=Warm/40kHz
    float    panel_temp_max_c;    // Cabinet fan turns ON above this
    float    panel_temp_hyst_c;   // Cabinet fan turns OFF below (max - hyst)
} controller_setpoints_t;

// Load setpoints from NVS (falls back to config.h defaults)
esp_err_t controller_init(void);

// Run one control cycle — call every SENSOR_READ_INTERVAL_MS
void controller_run_cycle(void);

// Atomic setpoint update + NVS persist
esp_err_t controller_set_setpoints(const controller_setpoints_t *sp);
void      controller_get_setpoints(controller_setpoints_t *out);

// Per-relay manual override — toggles relay to opposite of current state.
// The override is held until the automatic control logic makes a definitive
// decision (sensor threshold crossed or scheduled transition).
void controller_set_manual_toggle(relay_id_t relay);

// Force light to a specific brightness (0-100) regardless of schedule.
// Cancels any active sunrise/sunset ramp.  Override is held until the next
// scheduled on/off transition from light_should_be_on().
void controller_set_light_brightness(uint8_t pct);

// Copy current manual-override flags into caller's array.
void controller_get_manual_states(bool out[RELAY_COUNT]);

// Test mode — when enabled all schedule/hysteresis logic is suspended.
// Cleared on boot; exiting test mode also clears all manual overrides.
void controller_set_test_mode(bool enable);
bool controller_get_test_mode(void);

// Returns true when the door switch reports the door is currently open.
// Always false when DOOR_SWITCH_INSTALLED == 0.
bool controller_is_door_open(void);
// Raw GPB0 level from the last controller cycle (true=HIGH=door closed, false=LOW=open).
// Useful for diagnostics when the interlock appears stuck.
bool controller_get_door_pin_raw(void);
