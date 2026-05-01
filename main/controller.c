// ============================================================
//  controller.c — Environmental control logic
//  Hysteresis algorithm:
//    Heater:     OFF when temp >= setpoint + hyst
//                ON  when temp <= setpoint - hyst
//    Humidifier: OFF when RH   >= setpoint + hyst
//                ON  when RH   <= setpoint - hyst
//    Fan (CO2):  ON  when CO2  >= threshold
//                OFF when CO2  <  threshold - hyst
//    Light:      Follows schedule OR manual override
// ============================================================

#include "controller.h"
#include "sensor_manager.h"
#include "relay.h"
#include "mcp23017.h"
#include "light_pwm.h"
#include "config.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_sntp.h"
#include <time.h>
#include <string.h>

static const char *TAG = "controller";
static const char *NVS_NS  = "ctrl";

static controller_setpoints_t s_sp;
static bool s_manual[RELAY_COUNT];       // true = manual override active
static bool s_co2_fan = false;           // true = CO2 is currently driving the fan
static time_t s_fan_sched_start = 0;    // epoch time of current fan cycle start

static bool   s_light_rising  = false;  // sunrise ramp in progress
static bool   s_light_falling = false;  // sunset ramp in progress
static time_t s_light_transition_start = 0;
static bool   s_test_mode = false;      // test mode — schedules suspended
#if DOOR_SWITCH_INSTALLED
static bool   s_door_open    = false;   // true = door is currently open
static bool   s_door_pin_raw = true;    // last raw GPB0 level (true=HIGH=closed)
#endif
static SemaphoreHandle_t      s_sp_mutex;

// ============================================================
// NVS helpers
// ============================================================
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;

    // Read floats stored as uint32_t via memcpy
    uint32_t v32;
    size_t len = 4;

#define NVS_GET_FLOAT(key, field) \
    if (nvs_get_u32(h, key, &v32) == ESP_OK) memcpy(&s_sp.field, &v32, 4)

    NVS_GET_FLOAT("temp_sp",   temp_setpoint);
    NVS_GET_FLOAT("temp_hyst", temp_hysteresis);
    NVS_GET_FLOAT("hum_sp",    hum_setpoint);
    NVS_GET_FLOAT("hum_hyst",  hum_hysteresis);

    uint16_t co2;
    if (nvs_get_u16(h, "co2_thresh", &co2) == ESP_OK) s_sp.co2_threshold = co2;
    if (nvs_get_u16(h, "co2_hyst",   &co2) == ESP_OK) s_sp.co2_hysteresis = co2;

    int8_t ib;
    uint8_t ub;
    if (nvs_get_i8(h, "fan_on_m",  &ib) == ESP_OK) s_sp.fan_sched_on_min     = ib;
    if (nvs_get_i8(h, "fan_per_m", &ib) == ESP_OK) s_sp.fan_sched_period_min = ib;
    if (nvs_get_u8(h, "fan_sched", &ub) == ESP_OK) s_sp.fan_sched_enabled    = (bool)ub;

    if (nvs_get_i8(h, "light_on",     &ib) == ESP_OK) s_sp.light_on_hour  = ib;
    if (nvs_get_i8(h, "light_on_m",   &ib) == ESP_OK) s_sp.light_on_min   = ib;
    if (nvs_get_i8(h, "light_off",    &ib) == ESP_OK) s_sp.light_off_hour = ib;
    if (nvs_get_i8(h, "light_off_m",  &ib) == ESP_OK) s_sp.light_off_min  = ib;

    uint8_t ub2;
    if (nvs_get_u8(h, "light_rise_m", &ub2) == ESP_OK) s_sp.light_sunrise_min   = ub2;
    if (nvs_get_u8(h, "light_set_m",  &ub2) == ESP_OK) s_sp.light_sunset_min    = ub2;
    if (nvs_get_u8(h, "light_col",    &ub2) == ESP_OK) s_sp.light_colour_temp   = ub2;
    if (nvs_get_u8(h, "light_sched_en",&ub) == ESP_OK) s_sp.light_schedule_enabled = (bool)ub;
    if (nvs_get_u8(h, "light_max_br", &ub2) == ESP_OK) s_sp.light_max_brightness = ub2;

    uint32_t v32r;
#define NVS_GET_FLOAT2(key, field) \
    if (nvs_get_u32(h, key, &v32r) == ESP_OK) memcpy(&s_sp.field, &v32r, 4);
    NVS_GET_FLOAT2("pfan_max",  panel_temp_max_c)
    NVS_GET_FLOAT2("pfan_hyst", panel_temp_hyst_c)
#undef NVS_GET_FLOAT2

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    uint32_t v32;
#define NVS_SET_FLOAT(key, field) \
    memcpy(&v32, &s_sp.field, 4); nvs_set_u32(h, key, v32)

    NVS_SET_FLOAT("temp_sp",   temp_setpoint);
    NVS_SET_FLOAT("temp_hyst", temp_hysteresis);
    NVS_SET_FLOAT("hum_sp",    hum_setpoint);
    NVS_SET_FLOAT("hum_hyst",  hum_hysteresis);

    nvs_set_u16(h, "co2_thresh", s_sp.co2_threshold);
    nvs_set_u16(h, "co2_hyst",   s_sp.co2_hysteresis);
    nvs_set_i8(h,  "fan_on_m",   (int8_t)s_sp.fan_sched_on_min);
    nvs_set_i8(h,  "fan_per_m",  (int8_t)s_sp.fan_sched_period_min);
    nvs_set_u8(h,  "fan_sched",  (uint8_t)s_sp.fan_sched_enabled);
    nvs_set_i8(h,  "light_on",    (int8_t)s_sp.light_on_hour);
    nvs_set_i8(h,  "light_on_m",  (int8_t)s_sp.light_on_min);
    nvs_set_i8(h,  "light_off",   (int8_t)s_sp.light_off_hour);
    nvs_set_i8(h,  "light_off_m", (int8_t)s_sp.light_off_min);
    nvs_set_u8(h,  "light_rise_m",  (uint8_t)s_sp.light_sunrise_min);
    nvs_set_u8(h,  "light_set_m",   (uint8_t)s_sp.light_sunset_min);
    nvs_set_u8(h,  "light_col",     s_sp.light_colour_temp);
    nvs_set_u8(h,  "light_sched_en",(uint8_t)s_sp.light_schedule_enabled);
    nvs_set_u8(h,  "light_max_br",  s_sp.light_max_brightness);

    NVS_SET_FLOAT("pfan_max",  panel_temp_max_c);
    NVS_SET_FLOAT("pfan_hyst", panel_temp_hyst_c);

    nvs_commit(h);
    nvs_close(h);
}

// ============================================================
// Hysteresis helpers
// ============================================================
// Returns: 1 = should be ON, 0 = should be OFF, -1 = no change
static int hysteresis_check(float value, float setpoint, float hyst,
                             bool currently_on, bool invert)
{
    // invert=false: turn ON when value falls to/below setpoint (heater / humidifier)
    //               turn OFF when value rises to setpoint + hyst
    // invert=true:  turn ON when value rises to/above setpoint (fan / CO2)
    //               turn OFF when value falls to setpoint - hyst
    if (!invert) {
        if (currently_on  && value >= setpoint + hyst) return 0;
        if (!currently_on && value <= setpoint)        return 1;
    } else {
        if (currently_on  && value <= setpoint - hyst) return 0;
        if (!currently_on && value >= setpoint)        return 1;
    }
    return -1; // within band, no change
}

// ============================================================
// Light schedule helper
// ============================================================
static bool light_should_be_on(void)
{
    if (!s_sp.light_schedule_enabled) return false;

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);

    // If NTP hasn't synced yet (year == 1970) we can't trust the schedule.
    // Default to OFF so the light doesn't fire unexpectedly on every cold boot.
    if (t.tm_year + 1900 < 2024) return false;

    // Convert current time and schedule times to minutes-since-midnight for easy comparison
    int now_m = t.tm_hour * 60 + t.tm_min;
    int on_m  = s_sp.light_on_hour  * 60 + s_sp.light_on_min;
    int off_m = s_sp.light_off_hour * 60 + s_sp.light_off_min;

    if (on_m < off_m) {
        return (now_m >= on_m && now_m < off_m);
    } else {
        // Overnight schedule (e.g. 22:30 → 06:00)
        return (now_m >= on_m || now_m < off_m);
    }
}

// ============================================================
// Public API
// ============================================================
esp_err_t controller_init(void)
{
    s_sp_mutex = xSemaphoreCreateMutex();
    if (!s_sp_mutex) return ESP_ERR_NO_MEM;

    // Load defaults first, then override from NVS
    s_sp.temp_setpoint           = DEFAULT_TEMP_SETPOINT_C;
    s_sp.temp_hysteresis         = DEFAULT_TEMP_HYSTERESIS_C;
    s_sp.hum_setpoint            = DEFAULT_HUM_SETPOINT_PCT;
    s_sp.hum_hysteresis          = DEFAULT_HUM_HYSTERESIS_PCT;
    s_sp.co2_threshold           = DEFAULT_CO2_THRESHOLD_PPM;
    s_sp.co2_hysteresis          = DEFAULT_CO2_HYSTERESIS_PPM;
    s_sp.fan_sched_on_min        = DEFAULT_FAN_SCHED_ON_MIN;
    s_sp.fan_sched_period_min    = DEFAULT_FAN_SCHED_PERIOD_MIN;
    s_sp.fan_sched_enabled       = DEFAULT_FAN_SCHED_ENABLED;
    s_sp.light_on_hour           = DEFAULT_LIGHT_ON_HOUR;
    s_sp.light_on_min            = 0;
    s_sp.light_off_hour          = DEFAULT_LIGHT_OFF_HOUR;
    s_sp.light_off_min           = 0;
    s_sp.light_schedule_enabled  = true;
    s_sp.light_sunrise_min       = DEFAULT_LIGHT_SUNRISE_MIN;
    s_sp.light_sunset_min        = DEFAULT_LIGHT_SUNSET_MIN;
    s_sp.light_max_brightness    = DEFAULT_LIGHT_MAX_BRIGHTNESS;
    s_sp.panel_temp_max_c        = DEFAULT_PANEL_TEMP_MAX_C;
    s_sp.panel_temp_hyst_c       = DEFAULT_PANEL_TEMP_HYST_C;

    nvs_load();

    // Apply colour temperature at startup so the correct row group is active immediately.
    light_pwm_set_colour_temp(s_sp.light_colour_temp);

    ESP_LOGI(TAG, "Setpoints: Temp=%.1f±%.1f°C  RH=%.1f±%.1f%%  CO2=%u ppm",
             s_sp.temp_setpoint,  s_sp.temp_hysteresis,
             s_sp.hum_setpoint,   s_sp.hum_hysteresis,
             s_sp.co2_threshold);
    return ESP_OK;
}

void controller_set_test_mode(bool enable)
{
    s_test_mode = enable;
    if (!enable) {
        // Exiting test mode — clear all manual overrides and ramp state
        // so the schedule takes over cleanly on the very next cycle.
        for (int i = 0; i < RELAY_COUNT; i++) s_manual[i] = false;
        s_light_rising  = false;
        s_light_falling = false;
        ESP_LOGI(TAG, "Test mode OFF — manual overrides cleared, schedule resumed");
    } else {
        ESP_LOGI(TAG, "Test mode ON — schedules suspended");
    }
}

bool controller_get_test_mode(void)
{
    return s_test_mode;
}

void controller_run_cycle(void)
{
    // Safety watchdog — if sensors are stale, turn everything off
    if (!sensor_manager_is_healthy()) {
        ESP_LOGE(TAG, "Sensor watchdog expired — safe-off all relays");
        relay_all_off();
        light_pwm_set_all(0);
        s_light_rising  = false;
        s_light_falling = false;
        return;
    }

    // Test mode — skip all automatic control, let test page drive everything.
    if (s_test_mode) return;

    sensor_data_t d;
    sensor_manager_get(&d);
    float hum = sensor_manager_effective_humidity(&d);

    xSemaphoreTake(s_sp_mutex, portMAX_DELAY);
    controller_setpoints_t sp = s_sp;
    xSemaphoreGive(s_sp_mutex);
    // ── Panel fan (cabinet overtemp — always runs, door-independent) ──
    if (d.panel_temp_valid) {
        bool pfan = relay_get(RELAY_PANEL_FAN);
        if (!pfan && d.panel_temp_c >= sp.panel_temp_max_c) {
            relay_set(RELAY_PANEL_FAN, true);
            ESP_LOGW(TAG, "Panel fan ON -- cabinet %.1f C", d.panel_temp_c);
        } else if (pfan && d.panel_temp_c < sp.panel_temp_max_c - sp.panel_temp_hyst_c) {
            relay_set(RELAY_PANEL_FAN, false);
            ESP_LOGI(TAG, "Panel fan OFF -- cabinet %.1f C", d.panel_temp_c);
        }
    }

    // ── Door interlock ───────────────────────────────────────────
    // GPB0 with internal pull-up. Door open = LOW (DOOR_OPEN_LEVEL 0).
    // Set DOOR_SWITCH_INSTALLED 1 in config.h once physically wired.
#if DOOR_SWITCH_INSTALLED
    {
        bool door_pin = true;   // default HIGH (door closed) — safe on I2C error
        mcp23017_read_pin(1, MCP23017_GPB_DOOR_SWITCH, &door_pin);
        s_door_pin_raw = door_pin;  // always capture, even on I2C error (stays true)
        bool door_open = ((int)door_pin == DOOR_OPEN_LEVEL);
        ESP_LOGI(TAG, "Door: GPB0=%s  door_open=%d  s_door_open=%d",
                 door_pin ? "HIGH" : "LOW", (int)door_open, (int)s_door_open);
        if (door_open && !s_door_open) {
            ESP_LOGW(TAG, "Door opened -- lights ON, env control suspended");
            // Force env relays off; clear their manual overrides so auto resumes on close.
            relay_set(RELAY_HEATER,     false);  s_manual[RELAY_HEATER]     = false;
            relay_set(RELAY_HUMIDIFIER, false);  s_manual[RELAY_HUMIDIFIER] = false;
            relay_set(RELAY_FAN,        false);  s_manual[RELAY_FAN]        = false;
            // Force light on at full brightness; clear manual + ramps so close is clean.
            relay_set(RELAY_LIGHT,      true);   s_manual[RELAY_LIGHT]      = false;
            light_pwm_set_all(100);
            s_light_rising  = false;
            s_light_falling = false;
        } else if (!door_open && s_door_open) {
            ESP_LOGI(TAG, "Door closed -- resuming auto control");
            // Clear ALL manual overrides so the schedule takes effect on this cycle.
            for (int i = 0; i < RELAY_COUNT; i++) s_manual[i] = false;
            // Apply correct light state immediately — no ramp, door event is not a
            // scheduled transition so we don't want a 30-min sunset on a door close.
            s_light_rising  = false;
            s_light_falling = false;
            if (light_should_be_on()) {
                relay_set(RELAY_LIGHT, true);
                light_pwm_set_all(100);
            } else {
                light_pwm_set_all(0);
                relay_set(RELAY_LIGHT, false);
                ESP_LOGI(TAG, "Door closed, light OFF (schedule)");
            }
        }
        s_door_open = door_open;
        if (door_open) return;   // skip env control while door is open
    }
#endif
    // ── Heater ──────────────────────────────────────────────
    if (d.temp_rh_valid && d.temperature_c >= DEFAULT_TEMP_MAX_C) {
        s_manual[RELAY_HEATER] = false;
        relay_set(RELAY_HEATER, false);
        ESP_LOGW(TAG, "Heater safety cutoff at %.1f C", d.temperature_c);
    } else if (d.temp_rh_valid) {
        int r = hysteresis_check(d.temperature_c, sp.temp_setpoint,
                                 sp.temp_hysteresis,
                                 relay_get(RELAY_HEATER), false);
        if (r == 1 || r == 0) {
            // Auto has a definitive decision — clear any manual override
            s_manual[RELAY_HEATER] = false;
            relay_set(RELAY_HEATER, r == 1);
        }
        // r == -1: in dead-band, manual holds if set; else no change
    }

    // ── Humidifier ──────────────────────────────────────────────
    if (!relay_is_locked(RELAY_HUMIDIFIER)) {
        if (d.level_low) {
            // Water level interlock — empty reservoir, always takes priority
            if (relay_get(RELAY_HUMIDIFIER)) {
                s_manual[RELAY_HUMIDIFIER] = false;
                relay_set(RELAY_HUMIDIFIER, false);
                ESP_LOGW(TAG, "Humidifier OFF -- water low");
            }
        } else if (d.temp_rh_valid && hum >= DEFAULT_HUM_MAX_PCT) {
            // Safety cutoff always wins
            s_manual[RELAY_HUMIDIFIER] = false;
            relay_set(RELAY_HUMIDIFIER, false);
            ESP_LOGW(TAG, "Humidifier safety cutoff at %.1f%%", hum);
        } else if (relay_get(RELAY_FAN)) {
            // Fan interlock: don't humidify while extracting air.
            // Don't clear manual — override resumes once fan stops.
            relay_set(RELAY_HUMIDIFIER, false);
        } else if (d.temp_rh_valid) {
            int r = hysteresis_check(hum, sp.hum_setpoint,
                                     sp.hum_hysteresis,
                                     relay_get(RELAY_HUMIDIFIER), false);
            if (r == 1 || r == 0) {
                s_manual[RELAY_HUMIDIFIER] = false;
                relay_set(RELAY_HUMIDIFIER, r == 1);
            }
        }
    }

    // ── Fan ─────────────────────────────────────────────────
    {
        bool fan_on = relay_get(RELAY_FAN);
        uint16_t co2_off_pt = sp.co2_threshold > sp.co2_hysteresis
                              ? sp.co2_threshold - sp.co2_hysteresis : 0;

        if (d.co2_valid) {
            if (!s_co2_fan && d.co2_ppm >= sp.co2_threshold) {
                // CO2 too high → force fan ON, clear manual
                s_co2_fan = true;
                s_manual[RELAY_FAN] = false;
                relay_set(RELAY_FAN, true);
            } else if (s_co2_fan && d.co2_ppm < co2_off_pt) {
                // CO2 cleared hysteresis → hand back to schedule
                s_co2_fan = false;
            }
        }

        if (!s_co2_fan) {
            // CO2 is not in control — use timed schedule if enabled
            if (sp.fan_sched_enabled && sp.fan_sched_on_min > 0
                                     && sp.fan_sched_period_min > 0) {
                time_t now_t = time(NULL);
                if (s_fan_sched_start == 0) s_fan_sched_start = now_t;
                int period_sec = sp.fan_sched_period_min * 60;
                int elapsed    = (int)((now_t - s_fan_sched_start) % period_sec);
                bool sched_on  = (elapsed < sp.fan_sched_on_min * 60);
                if (sched_on != fan_on) {
                    // Scheduled transition clears manual override
                    s_manual[RELAY_FAN] = false;
                    relay_set(RELAY_FAN, sched_on);
                }
                // If fan_on == sched_on: already correct, manual holds if set
            } else if (!s_manual[RELAY_FAN] && fan_on) {
                // No schedule, CO2 cleared: turn fan off
                relay_set(RELAY_FAN, false);
            }
        }
    }

    // ── Light (PWM dimming with sunrise/sunset ramp) ────────
    {
        bool   light_target = light_should_be_on();
        bool   light_on     = relay_get(RELAY_LIGHT);
        time_t now_t        = time(NULL);

        // Suppress ramps if the on/off window is too short to be worth ramping.
        // Threshold: total ramp time * 4 (e.g. 30+30 min ramps → skip if window < 240 min).
        int on_m  = sp.light_on_hour  * 60 + sp.light_on_min;
        int off_m = sp.light_off_hour * 60 + sp.light_off_min;
        int window_min = (off_m > on_m) ? (off_m - on_m)
                                        : (1440 - on_m + off_m);  // overnight
        int ramp_total = sp.light_sunrise_min + sp.light_sunset_min;
        bool use_ramps = (ramp_total > 0) && (window_min >= ramp_total * 4);
        int eff_sunrise = use_ramps ? sp.light_sunrise_min : 0;
        int eff_sunset  = use_ramps ? sp.light_sunset_min  : 0;
        uint8_t max_br  = sp.light_max_brightness ? sp.light_max_brightness : 100;

        if (s_manual[RELAY_LIGHT]) {
            // Manual override active — sunrise/sunset not running.
            // Nothing to do here; relay and PWM were set in controller_set_manual_toggle.

        } else if (light_target && s_light_falling) {
            // Schedule flipped back ON while sunset ramp was in progress — abort and rise.
            s_light_falling = false;
            relay_set(RELAY_LIGHT, true);
            if (eff_sunrise <= 0) {
                light_pwm_set_all(max_br);
                ESP_LOGI(TAG, "Light ON (instant, aborted sunset)");
            } else {
                s_light_rising = true;
                s_light_transition_start = now_t;
                light_pwm_set_all(1);
                ESP_LOGI(TAG, "Light ON — sunrise over %d min (aborted sunset)", eff_sunrise);
            }

        } else if (light_target && !light_on && !s_light_rising && !s_light_falling) {
            // Schedule just turned ON — enable relay, begin sunrise.
            s_manual[RELAY_LIGHT] = false;
            relay_set(RELAY_LIGHT, true);
            s_light_falling = false;
            if (eff_sunrise <= 0) {
                light_pwm_set_all(max_br);
                s_light_rising = false;
                ESP_LOGI(TAG, "Light ON (instant)");
            } else {
                s_light_rising = true;
                s_light_transition_start = now_t;
                light_pwm_set_all(1);  // start at 1% so panel receives power
                ESP_LOGI(TAG, "Light ON — sunrise over %d min", eff_sunrise);
            }

        } else if (!light_target && (light_on || s_light_rising) && !s_light_falling) {
            // Schedule just turned OFF — begin sunset.
            s_manual[RELAY_LIGHT] = false;
            s_light_rising = false;
            if (eff_sunset <= 0) {
                light_pwm_set_all(0);
                relay_set(RELAY_LIGHT, false);
                ESP_LOGI(TAG, "Light OFF (instant)");
            } else {
                s_light_falling = true;
                s_light_transition_start = now_t;
                // Relay stays ON until ramp completes so panel receives power.
                ESP_LOGI(TAG, "Light OFF — sunset over %d min", eff_sunset);
            }

        } else if (s_light_rising) {
            // Sunrise ramp in progress.
            int elapsed_min = (int)((now_t - s_light_transition_start) / 60);
            if (elapsed_min >= eff_sunrise) {
                light_pwm_set_all(max_br);
                s_light_rising = false;
                ESP_LOGI(TAG, "Sunrise complete");
            } else {
                uint8_t pct = (uint8_t)(elapsed_min * max_br / eff_sunrise);
                light_pwm_set_all(pct < 1 ? 1 : pct);
            }

        } else if (s_light_falling) {
            // Sunset ramp in progress.
            int elapsed_min = (int)((now_t - s_light_transition_start) / 60);
            if (elapsed_min >= eff_sunset) {
                light_pwm_set_all(0);
                relay_set(RELAY_LIGHT, false);
                s_light_falling = false;
                ESP_LOGI(TAG, "Sunset complete");
            } else {
                uint8_t pct = (uint8_t)(max_br - elapsed_min * max_br / eff_sunset);
                light_pwm_set_all(pct);
            }
        }
        // else: stable state, no change needed.
    }

    // ── Cycle diagnostics ───────────────────────────────────
    ESP_LOGI(TAG, "Cycle: T=%.1f°C(sp=%.1f±%.1f) H=%.1f%%(sp=%.1f±%.1f) CO2=%uppm%s"
                  " | Heater=%s Hum=%s Fan=%s PFan=%s Light=%s Panel=%.1f°C%s",
             d.temperature_c,    sp.temp_setpoint, sp.temp_hysteresis,
             hum,                sp.hum_setpoint,  sp.hum_hysteresis,
             d.co2_ppm, d.co2_valid ? "" : "(no CO2)",
             relay_get(RELAY_HEATER)     ? "ON " : "off",
             relay_get(RELAY_HUMIDIFIER) ? "ON " : "off",
             relay_get(RELAY_FAN)        ? "ON " : "off",
             relay_get(RELAY_PANEL_FAN)  ? "ON " : "off",
             relay_get(RELAY_LIGHT)      ? "ON " : "off",
             d.panel_temp_c, d.panel_temp_valid ? "" : "(no sensor)");
}

esp_err_t controller_set_setpoints(const controller_setpoints_t *sp)
{
    if (!sp) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_sp_mutex, portMAX_DELAY);
    s_sp = *sp;
    xSemaphoreGive(s_sp_mutex);
    nvs_save();
    light_pwm_set_colour_temp(sp->light_colour_temp);
    ESP_LOGI(TAG, "Setpoints updated and saved");
    return ESP_OK;
}

void controller_get_setpoints(controller_setpoints_t *out)
{
    xSemaphoreTake(s_sp_mutex, portMAX_DELAY);
    *out = s_sp;
    xSemaphoreGive(s_sp_mutex);
}

void controller_set_manual_toggle(relay_id_t relay)
{
    if (relay >= RELAY_COUNT) return;
    bool current = relay_get(relay);
    bool new_state = !current;
    s_manual[relay] = true;
    relay_set(relay, new_state);   // flip to opposite state immediately
    if (relay == RELAY_LIGHT) {
        // Cancel any in-progress ramp and apply full on/off immediately.
        s_light_rising  = false;
        s_light_falling = false;
        light_pwm_set_all(new_state ? 100 : 0);
    }
    const char *rname = relay == RELAY_HEATER     ? "Heater"     :
                        relay == RELAY_HUMIDIFIER ? "Humidifier" :
                        relay == RELAY_FAN        ? "Fan"        :
                        relay == RELAY_PANEL_FAN  ? "PanelFan"   : "Light";
    ESP_LOGI(TAG, "%s manual override -> %s", rname, new_state ? "ON" : "OFF");
}

void controller_set_light_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    s_light_rising  = false;
    s_light_falling = false;
    relay_set(RELAY_LIGHT, pct > 0);
    light_pwm_set_all(pct);
    s_manual[RELAY_LIGHT] = true;   // hold until next scheduled transition
    ESP_LOGI(TAG, "Light brightness forced to %d%%", pct);
}

void controller_get_manual_states(bool out[RELAY_COUNT])
{
    for (int i = 0; i < RELAY_COUNT; i++) out[i] = s_manual[i];
}

bool controller_is_door_open(void)
{
#if DOOR_SWITCH_INSTALLED
    return s_door_open;
#else
    return false;
#endif
}

// Raw GPB0 level from the last controller cycle (true=HIGH=door closed).
bool controller_get_door_pin_raw(void)
{
#if DOOR_SWITCH_INSTALLED
    return s_door_pin_raw;
#else
    return true;   // pull-up default
#endif
}
