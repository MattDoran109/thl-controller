#pragma once
// ============================================================
//  wifi_manager.h — WiFi credential NVS storage + mode state
// ============================================================
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// NVS namespace used for all WiFi + device-identity keys
#define WIFI_NVS_NS  "wifi"

// Maximum field lengths (including null terminator)
#define WIFI_SSID_MAX    33
#define WIFI_PASS_MAX    65
#define WIFI_HOST_MAX    33
#define WIFI_TZ_MAX      48
#define WIFI_DNAME_MAX   33

// Current operating mode
typedef enum {
    WM_MODE_STA_PRIMARY  = 0,  // connected to primary network
    WM_MODE_STA_FALLBACK = 1,  // connected to fallback network
    WM_MODE_SOFTAP       = 2,  // SoftAP — no internet
    WM_MODE_OFFLINE      = 3,  // all attempts failed, AP also not up
} wifi_op_mode_t;

// WiFi + device config loaded from NVS
typedef struct {
    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_PASS_MAX];
    char ssid2[WIFI_SSID_MAX];
    char pass2[WIFI_PASS_MAX];
    char hostname[WIFI_HOST_MAX];
    char device_name[WIFI_DNAME_MAX];
    char timezone[WIFI_TZ_MAX];
} wifi_config_nvs_t;

// Load config from NVS; falls back to config.h defaults for missing keys
void wifi_manager_load(wifi_config_nvs_t *out);

// Persist config to NVS
esp_err_t wifi_manager_save(const wifi_config_nvs_t *cfg);

// Erase all keys in the wifi NVS namespace (part of factory reset)
esp_err_t wifi_manager_erase(void);

// Get/set current operating mode (set by main.c after wifi_init)
void           wifi_manager_set_mode(wifi_op_mode_t mode);
wifi_op_mode_t wifi_manager_get_mode(void);

// Return current IP as a dotted-decimal string (e.g. "192.168.1.42")
// Returns "0.0.0.0" when in AP mode or not connected.
void wifi_manager_get_ip(char *buf, size_t len);
void wifi_manager_set_ip(const char *ip_str);
