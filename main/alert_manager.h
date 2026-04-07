#pragma once
// ============================================================
//  alert_manager.h — configurable push-notification alerts
// ============================================================
#include <stdbool.h>
#include <stdint.h>

// Per-alert configuration.  All repeat intervals are in seconds.
typedef struct {
    // Temperature (°C)
    uint8_t  temp_en;
    float    temp_lo;
    float    temp_hi;
    uint32_t temp_rep;

    // Humidity (%)
    uint8_t  hum_en;
    float    hum_lo;
    float    hum_hi;
    uint32_t hum_rep;

    // CO2 (ppm)
    uint8_t  co2_en;
    uint16_t co2_lo;
    uint16_t co2_hi;
    uint32_t co2_rep;

    // Water level (low = alarm)
    uint8_t  water_en;
    uint32_t water_rep;

    // Door open duration
    uint8_t  door_en;
    uint32_t door_dur;   // seconds before first alert fires
    uint32_t door_rep;   // repeat interval while door stays open

    // SD card not mounted
    uint8_t  sd_en;
    uint32_t sd_rep;

    // WiFi not connected
    uint8_t  wifi_en;
    uint32_t wifi_rep;

    // Equipment failure — relay ON but unable to reach setpoint after N minutes
    uint8_t  temp_fail_en;
    uint32_t temp_fail_min;  // minutes before first alert
    uint32_t temp_fail_rep;  // repeat interval in seconds

    uint8_t  hum_fail_en;
    uint32_t hum_fail_min;
    uint32_t hum_fail_rep;

    uint8_t  co2_fail_en;
    uint32_t co2_fail_min;   // minutes CO2 stays above threshold before alert
    uint32_t co2_fail_rep;

    // Send single notification on boot (power-restored / reboot)
    uint8_t  reboot_en;
} alert_cfg_t;

// Load config from NVS.  Call once at startup, after notifier_init().
void alert_manager_init(void);

// Evaluate all alert conditions.  Called from sensor_task each cycle.
// Internally calls sensor_manager_get(), controller_is_door_open(), etc.
void alert_manager_check(void);

// Get / set the current alert configuration (also persists to NVS on set).
void alert_manager_get_cfg(alert_cfg_t *out);
void alert_manager_set_cfg(const alert_cfg_t *cfg);
