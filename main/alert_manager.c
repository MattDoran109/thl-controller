// ============================================================
//  alert_manager.c — configurable push-notification alerts
// ============================================================

#include "alert_manager.h"
#include "sensor_manager.h"
#include "controller.h"
#include "relay.h"
#include "sd_logger.h"
#include "wifi_manager.h"
#include "notifier.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <time.h>
#include <string.h>
#include <stdio.h>

#define TAG    "alerts"
#define NVS_NS "alerts"

// ── defaults ─────────────────────────────────────────────────
static const alert_cfg_t k_defaults = {
    .temp_en   = 0, .temp_lo  = 15.0f, .temp_hi  = 35.0f, .temp_rep  = 3600,
    .hum_en    = 0, .hum_lo   = 30.0f, .hum_hi   = 85.0f, .hum_rep   = 3600,
    .co2_en    = 0, .co2_lo   = 400,   .co2_hi   = 2000,  .co2_rep   = 3600,
    .water_en  = 0, .water_rep = 300,
    .door_en   = 0, .door_dur = 60,    .door_rep  = 30,
    .sd_en     = 0, .sd_rep   = 3600,
    .wifi_en   = 0, .wifi_rep = 3600,
    .temp_fail_en = 0, .temp_fail_min = 30,  .temp_fail_rep = 3600,
    .hum_fail_en  = 0, .hum_fail_min  = 30,  .hum_fail_rep  = 3600,
    .co2_fail_en  = 0, .co2_fail_min  = 15,  .co2_fail_rep  = 3600,
    .reboot_en = 1,
};

// ── runtime state ─────────────────────────────────────────────
typedef enum {
    ALT_TEMP  = 0,
    ALT_HUM,
    ALT_CO2,
    ALT_WATER,
    ALT_DOOR,
    ALT_SD,
    ALT_WIFI,
    ALT_TEMP_FAIL,
    ALT_HUM_FAIL,
    ALT_CO2_FAIL,
    ALT_COUNT,
} alert_idx_t;

static alert_cfg_t s_cfg;
static time_t      s_last[ALT_COUNT]; // epoch of last send; 0 = never / reset
static time_t      s_door_opened_at;  // epoch when door first opened; 0 = closed
static time_t      s_heater_fail_since; // epoch heater turned ON below setpoint
static time_t      s_hum_fail_since;    // epoch humidifier turned ON below setpoint
static time_t      s_co2_fail_since;    // epoch CO2 rose above threshold
static bool        s_reboot_sent;     // reboot notification delivered this boot

// ── NVS load / save ──────────────────────────────────────────
#define NVS_GET_U8(key, field) \
    if (nvs_get_u8 (h, key, &u8 ) == ESP_OK) s_cfg.field = u8
#define NVS_GET_U16(key, field) \
    if (nvs_get_u16(h, key, &u16) == ESP_OK) s_cfg.field = u16
#define NVS_GET_U32(key, field) \
    if (nvs_get_u32(h, key, &u32) == ESP_OK) s_cfg.field = u32
#define NVS_GET_FLOAT(key, field) \
    if (nvs_get_u32(h, key, &u32) == ESP_OK) memcpy(&s_cfg.field, &u32, 4)

#define NVS_SET_U8(key, field)  nvs_set_u8 (h, key, s_cfg.field)
#define NVS_SET_U16(key, field) nvs_set_u16(h, key, s_cfg.field)
#define NVS_SET_U32(key, field) nvs_set_u32(h, key, s_cfg.field)
#define NVS_SET_FLOAT(key, field) \
    do { uint32_t _v; memcpy(&_v, &s_cfg.field, 4); nvs_set_u32(h, key, _v); } while (0)

static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8; uint16_t u16; uint32_t u32;

    NVS_GET_U8   ("al_t_en",  temp_en);
    NVS_GET_FLOAT("al_t_lo",  temp_lo);
    NVS_GET_FLOAT("al_t_hi",  temp_hi);
    NVS_GET_U32  ("al_t_rep", temp_rep);

    NVS_GET_U8   ("al_h_en",  hum_en);
    NVS_GET_FLOAT("al_h_lo",  hum_lo);
    NVS_GET_FLOAT("al_h_hi",  hum_hi);
    NVS_GET_U32  ("al_h_rep", hum_rep);

    NVS_GET_U8   ("al_c_en",  co2_en);
    NVS_GET_U16  ("al_c_lo",  co2_lo);
    NVS_GET_U16  ("al_c_hi",  co2_hi);
    NVS_GET_U32  ("al_c_rep", co2_rep);

    NVS_GET_U8   ("al_w_en",  water_en);
    NVS_GET_U32  ("al_w_rep", water_rep);

    NVS_GET_U8   ("al_d_en",  door_en);
    NVS_GET_U32  ("al_d_dur", door_dur);
    NVS_GET_U32  ("al_d_rep", door_rep);

    NVS_GET_U8   ("al_sd_en", sd_en);
    NVS_GET_U32  ("al_sd_rep",sd_rep);

    NVS_GET_U8   ("al_wf_en", wifi_en);
    NVS_GET_U32  ("al_wf_rep",wifi_rep);

    NVS_GET_U8   ("al_rb_en", reboot_en);

    NVS_GET_U8   ("al_tf_en",  temp_fail_en);
    NVS_GET_U32  ("al_tf_min", temp_fail_min);
    NVS_GET_U32  ("al_tf_rep", temp_fail_rep);
    NVS_GET_U8   ("al_hf_en",  hum_fail_en);
    NVS_GET_U32  ("al_hf_min", hum_fail_min);
    NVS_GET_U32  ("al_hf_rep", hum_fail_rep);
    NVS_GET_U8   ("al_cf_en",  co2_fail_en);
    NVS_GET_U32  ("al_cf_min", co2_fail_min);
    NVS_GET_U32  ("al_cf_rep", co2_fail_rep);

    nvs_close(h);
}

static void nvs_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;

    NVS_SET_U8   ("al_t_en",  temp_en);
    NVS_SET_FLOAT("al_t_lo",  temp_lo);
    NVS_SET_FLOAT("al_t_hi",  temp_hi);
    NVS_SET_U32  ("al_t_rep", temp_rep);

    NVS_SET_U8   ("al_h_en",  hum_en);
    NVS_SET_FLOAT("al_h_lo",  hum_lo);
    NVS_SET_FLOAT("al_h_hi",  hum_hi);
    NVS_SET_U32  ("al_h_rep", hum_rep);

    NVS_SET_U8   ("al_c_en",  co2_en);
    NVS_SET_U16  ("al_c_lo",  co2_lo);
    NVS_SET_U16  ("al_c_hi",  co2_hi);
    NVS_SET_U32  ("al_c_rep", co2_rep);

    NVS_SET_U8   ("al_w_en",  water_en);
    NVS_SET_U32  ("al_w_rep", water_rep);

    NVS_SET_U8   ("al_d_en",  door_en);
    NVS_SET_U32  ("al_d_dur", door_dur);
    NVS_SET_U32  ("al_d_rep", door_rep);

    NVS_SET_U8   ("al_sd_en", sd_en);
    NVS_SET_U32  ("al_sd_rep",sd_rep);

    NVS_SET_U8   ("al_wf_en", wifi_en);
    NVS_SET_U32  ("al_wf_rep",wifi_rep);

    NVS_SET_U8   ("al_rb_en", reboot_en);

    NVS_SET_U8   ("al_tf_en",  temp_fail_en);
    NVS_SET_U32  ("al_tf_min", temp_fail_min);
    NVS_SET_U32  ("al_tf_rep", temp_fail_rep);
    NVS_SET_U8   ("al_hf_en",  hum_fail_en);
    NVS_SET_U32  ("al_hf_min", hum_fail_min);
    NVS_SET_U32  ("al_hf_rep", hum_fail_rep);
    NVS_SET_U8   ("al_cf_en",  co2_fail_en);
    NVS_SET_U32  ("al_cf_min", co2_fail_min);
    NVS_SET_U32  ("al_cf_rep", co2_fail_rep);

    nvs_commit(h);
    nvs_close(h);
}

// ── cooldown helper ───────────────────────────────────────────
// in_alarm=true  → send if cooldown has elapsed; return true if sent
// in_alarm=false → reset timer so next alarm fires immediately; return false
static bool maybe_send(alert_idx_t idx, bool in_alarm, uint32_t rep_s,
                        const char *title, const char *msg)
{
    if (!in_alarm) {
        s_last[idx] = 0;   // reset: next alarm entry fires without delay
        return false;
    }
    time_t now = time(NULL);
    if (now < 1700000000L) return false;  // RTC not yet synced, skip
    if ((now - s_last[idx]) >= (time_t)rep_s) {
        notifier_send(title, msg);
        s_last[idx] = now;
        return true;
    }
    return false;
}

// ── public API ────────────────────────────────────────────────
void alert_manager_init(void)
{
    s_cfg = k_defaults;
    nvs_load();
    memset(s_last, 0, sizeof(s_last));
    s_door_opened_at     = 0;
    s_heater_fail_since  = 0;
    s_hum_fail_since     = 0;
    s_co2_fail_since     = 0;
    s_reboot_sent        = false;
    ESP_LOGI(TAG, "alert manager ready (reboot_en=%d)", s_cfg.reboot_en);
}

void alert_manager_check(void)
{
    sensor_data_t sd;
    sensor_manager_get(&sd);
    time_t now    = time(NULL);
    bool   rtc_ok = (now > 1700000000L);

    // Fetch setpoints once — used by equipment-failure checks below.
    controller_setpoints_t sp;
    controller_get_setpoints(&sp);

    // ── Power-restored / reboot alert ────────────────────────
    // Wait until WiFi is up so the first POST can succeed.
    if (!s_reboot_sent && s_cfg.reboot_en) {
        wifi_op_mode_t m = wifi_manager_get_mode();
        if (m == WM_MODE_STA_PRIMARY || m == WM_MODE_STA_FALLBACK) {
            wifi_config_nvs_t wcfg;
            wifi_manager_load(&wcfg);
            const char *dname = wcfg.device_name[0] ? wcfg.device_name : "THL Controller";
            char reboot_msg[80];
            snprintf(reboot_msg, sizeof(reboot_msg), "%s has restarted", dname);
            notifier_send("Power Restored", reboot_msg);
            s_reboot_sent = true;
        }
    }

    // ── Temperature ──────────────────────────────────────────
    if (s_cfg.temp_en && sd.temp_rh_valid) {
        bool hi = sd.temperature_c > s_cfg.temp_hi;
        bool lo = sd.temperature_c < s_cfg.temp_lo;
        if (hi || lo) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Temperature %.1f\xc2\xb0" "C %s %.1f\xc2\xb0" "C",
                     sd.temperature_c, hi ? "above" : "below",
                     hi ? s_cfg.temp_hi : s_cfg.temp_lo);
            maybe_send(ALT_TEMP, true, s_cfg.temp_rep, "Temperature Alert", msg);
        } else {
            maybe_send(ALT_TEMP, false, s_cfg.temp_rep, NULL, NULL);
        }
    }

    // ── Humidity ─────────────────────────────────────────────
    if (s_cfg.hum_en && sd.temp_rh_valid) {
        float hum = sensor_manager_effective_humidity(&sd);
        bool  hi  = hum > s_cfg.hum_hi;
        bool  lo  = hum < s_cfg.hum_lo;
        if (hi || lo) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Humidity %.1f%% %s %.1f%%",
                     hum, hi ? "above" : "below",
                     hi ? s_cfg.hum_hi : s_cfg.hum_lo);
            maybe_send(ALT_HUM, true, s_cfg.hum_rep, "Humidity Alert", msg);
        } else {
            maybe_send(ALT_HUM, false, s_cfg.hum_rep, NULL, NULL);
        }
    }

    // ── CO2 ──────────────────────────────────────────────────
    if (s_cfg.co2_en && sd.co2_valid) {
        bool hi = sd.co2_ppm > s_cfg.co2_hi;
        bool lo = sd.co2_ppm < s_cfg.co2_lo;
        if (hi || lo) {
            char msg[96];
            snprintf(msg, sizeof(msg), "CO2 %u ppm %s %u ppm",
                     sd.co2_ppm, hi ? "above" : "below",
                     hi ? s_cfg.co2_hi : s_cfg.co2_lo);
            maybe_send(ALT_CO2, true, s_cfg.co2_rep, "CO2 Alert", msg);
        } else {
            maybe_send(ALT_CO2, false, s_cfg.co2_rep, NULL, NULL);
        }
    }

    // ── Equipment failure: heater ON but not reaching setpoint ─
    // Timer persists through fan-cycle relay-off periods; only resets when condition improves.
    if (s_cfg.temp_fail_en && sd.temp_rh_valid && rtc_ok) {
        bool heater_on = relay_get(RELAY_HEATER);
        bool below_sp  = sd.temperature_c < sp.temp_setpoint;
        if (heater_on && below_sp) {
            if (s_heater_fail_since == 0) s_heater_fail_since = now;
        } else if (!below_sp) {
            /* condition resolved — reset */
            s_heater_fail_since = 0;
            maybe_send(ALT_TEMP_FAIL, false, s_cfg.temp_fail_rep, NULL, NULL);
        }
        /* relay off + still below setpoint (fan cycle): hold timer, don't fire */
        if (heater_on && below_sp && s_heater_fail_since > 0) {
            time_t elapsed = now - s_heater_fail_since;
            if (elapsed >= (time_t)(s_cfg.temp_fail_min * 60)) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "Heater on %ld min, temp %.1f\xc2\xb0" "C \xe2\x80\x94 target %.1f\xc2\xb0" "C",
                         (long)(elapsed / 60), sd.temperature_c, sp.temp_setpoint);
                maybe_send(ALT_TEMP_FAIL, true, s_cfg.temp_fail_rep, "Heater Fault", msg);
            }
        }
    }

    // ── Equipment failure: humidifier ON but not reaching setpoint ─
    // Timer persists through fan-cycle relay-off periods; only resets when condition improves.
    if (s_cfg.hum_fail_en && sd.temp_rh_valid && rtc_ok) {
        float hum          = sensor_manager_effective_humidity(&sd);
        bool  humidifier   = relay_get(RELAY_HUMIDIFIER);
        bool  below_sp     = hum < sp.hum_setpoint;
        if (humidifier && below_sp) {
            if (s_hum_fail_since == 0) s_hum_fail_since = now;
        } else if (!below_sp) {
            /* condition resolved — reset */
            s_hum_fail_since = 0;
            maybe_send(ALT_HUM_FAIL, false, s_cfg.hum_fail_rep, NULL, NULL);
        }
        /* relay off + still below setpoint (fan cycle): hold timer, don't fire */
        if (humidifier && below_sp && s_hum_fail_since > 0) {
            time_t elapsed = now - s_hum_fail_since;
            if (elapsed >= (time_t)(s_cfg.hum_fail_min * 60)) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "Humidifier on %ld min, humidity %.1f%% — target %.1f%%",
                         (long)(elapsed / 60), hum, sp.hum_setpoint);
                maybe_send(ALT_HUM_FAIL, true, s_cfg.hum_fail_rep, "Humidifier Fault", msg);
            }
        }
    }

    // ── Equipment failure: CO2 above threshold for too long ───
    if (s_cfg.co2_fail_en && sd.co2_valid && rtc_ok) {
        bool co2_high = (sd.co2_ppm >= sp.co2_threshold);
        if (co2_high) {
            if (s_co2_fail_since == 0) s_co2_fail_since = now;
            time_t elapsed = now - s_co2_fail_since;
            if (elapsed >= (time_t)(s_cfg.co2_fail_min * 60)) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "CO2 at %u ppm for %ld min — extraction may have failed",
                         sd.co2_ppm, (long)(elapsed / 60));
                maybe_send(ALT_CO2_FAIL, true, s_cfg.co2_fail_rep, "CO2 Extraction Fault", msg);
            }
        } else {
            s_co2_fail_since = 0;
            maybe_send(ALT_CO2_FAIL, false, s_cfg.co2_fail_rep, NULL, NULL);
        }
    }

    // ── Water level ──────────────────────────────────────────
    if (s_cfg.water_en) {
        if (sd.level_low) {
            maybe_send(ALT_WATER, true, s_cfg.water_rep,
                       "Water Level Alert", "Water level is LOW — refill required");
        } else {
            maybe_send(ALT_WATER, false, s_cfg.water_rep, NULL, NULL);
        }
    }

    // ── Door open ────────────────────────────────────────────
    if (s_cfg.door_en) {
        bool door_open = controller_is_door_open();
        if (door_open) {
            if (s_door_opened_at == 0)
                s_door_opened_at = now;
            time_t elapsed = now - s_door_opened_at;
            if (rtc_ok && elapsed >= (time_t)s_cfg.door_dur) {
                char msg[80];
                snprintf(msg, sizeof(msg),
                         "Door has been open for %ld seconds", (long)elapsed);
                maybe_send(ALT_DOOR, true, s_cfg.door_rep, "Door Alert", msg);
            }
        } else {
            s_door_opened_at = 0;
            maybe_send(ALT_DOOR, false, s_cfg.door_rep, NULL, NULL);
        }
    }

    // ── SD card failure ──────────────────────────────────────
    if (s_cfg.sd_en) {
        bool mounted = sd_logger_is_mounted();
        if (!mounted) {
            maybe_send(ALT_SD, true, s_cfg.sd_rep,
                       "SD Card Alert", "SD card is not mounted — logging inactive");
        } else {
            maybe_send(ALT_SD, false, s_cfg.sd_rep, NULL, NULL);
        }
    }

    // ── WiFi failure ─────────────────────────────────────────
    if (s_cfg.wifi_en) {
        wifi_op_mode_t mode = wifi_manager_get_mode();
        bool connected = (mode == WM_MODE_STA_PRIMARY || mode == WM_MODE_STA_FALLBACK);
        if (!connected) {
            maybe_send(ALT_WIFI, true, s_cfg.wifi_rep,
                       "WiFi Alert", "WiFi is not connected");
        } else {
            maybe_send(ALT_WIFI, false, s_cfg.wifi_rep, NULL, NULL);
        }
    }
}

void alert_manager_get_cfg(alert_cfg_t *out)
{
    if (out) *out = s_cfg;
}

void alert_manager_set_cfg(const alert_cfg_t *cfg)
{
    if (!cfg) return;
    s_cfg = *cfg;
    // Clamp repeat intervals to sane minimums
    if (s_cfg.temp_rep  < 60)  s_cfg.temp_rep  = 60;
    if (s_cfg.hum_rep   < 60)  s_cfg.hum_rep   = 60;
    if (s_cfg.co2_rep   < 60)  s_cfg.co2_rep   = 60;
    if (s_cfg.water_rep < 60)  s_cfg.water_rep  = 60;
    if (s_cfg.door_dur  < 5)   s_cfg.door_dur   = 5;
    if (s_cfg.door_rep  < 10)  s_cfg.door_rep   = 10;
    if (s_cfg.sd_rep    < 60)  s_cfg.sd_rep     = 60;
    if (s_cfg.wifi_rep  < 60)  s_cfg.wifi_rep   = 60;
    if (s_cfg.temp_fail_min < 5)   s_cfg.temp_fail_min = 5;
    if (s_cfg.temp_fail_rep < 60)  s_cfg.temp_fail_rep = 60;
    if (s_cfg.hum_fail_min  < 5)   s_cfg.hum_fail_min  = 5;
    if (s_cfg.hum_fail_rep  < 60)  s_cfg.hum_fail_rep  = 60;
    if (s_cfg.co2_fail_min  < 5)   s_cfg.co2_fail_min  = 5;
    if (s_cfg.co2_fail_rep  < 60)  s_cfg.co2_fail_rep  = 60;
    nvs_save();
    ESP_LOGI(TAG, "alert config saved");
}
