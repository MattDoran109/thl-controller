// ============================================================
//  wifi_manager.c — WiFi credential NVS storage + mode state
// ============================================================
#include "wifi_manager.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_mgr";

static wifi_op_mode_t s_mode = WM_MODE_OFFLINE;
static char s_ip[16] = "0.0.0.0";

// --------------------------------------------------------
// NVS helpers
// --------------------------------------------------------
#define NVS_GET_STR(h, key, dst, maxlen) \
    do { size_t _l = (maxlen); \
         if (nvs_get_str((h), (key), (dst), &_l) != ESP_OK) (dst)[0] = '\0'; } while(0)

void wifi_manager_load(wifi_config_nvs_t *out)
{
    // Start with compile-time defaults
    strlcpy(out->ssid,        WIFI_SSID_DEFAULT,   sizeof(out->ssid));
    strlcpy(out->pass,        WIFI_PASS_DEFAULT,   sizeof(out->pass));
    strlcpy(out->ssid2,       WIFI_SSID2_DEFAULT,  sizeof(out->ssid2));
    strlcpy(out->pass2,       WIFI_PASS2_DEFAULT,  sizeof(out->pass2));
    strlcpy(out->hostname,    WIFI_HOSTNAME,        sizeof(out->hostname));
    strlcpy(out->device_name, DEVICE_NAME_DEFAULT,  sizeof(out->device_name));
    strlcpy(out->timezone,    TIMEZONE,             sizeof(out->timezone));

    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "No wifi NVS namespace — using defaults");
        return;
    }

    char tmp[WIFI_PASS_MAX];

    NVS_GET_STR(h, "ssid",   tmp, sizeof(tmp));
    if (tmp[0]) strlcpy(out->ssid, tmp, sizeof(out->ssid));

    NVS_GET_STR(h, "pass",   tmp, sizeof(tmp));
    if (tmp[0]) strlcpy(out->pass, tmp, sizeof(out->pass));

    NVS_GET_STR(h, "ssid2",  tmp, sizeof(tmp));
    if (tmp[0]) strlcpy(out->ssid2, tmp, sizeof(out->ssid2));
    else out->ssid2[0] = '\0';  // explicit empty means "none configured"

    NVS_GET_STR(h, "pass2",  tmp, sizeof(tmp));
    strlcpy(out->pass2, tmp, sizeof(out->pass2));

    NVS_GET_STR(h, "hostname", tmp, sizeof(tmp));
    if (tmp[0]) strlcpy(out->hostname, tmp, sizeof(out->hostname));

    NVS_GET_STR(h, "dname",  tmp, sizeof(tmp));
    if (tmp[0]) strlcpy(out->device_name, tmp, sizeof(out->device_name));

    NVS_GET_STR(h, "tz",     tmp, sizeof(tmp));
    if (tmp[0]) strlcpy(out->timezone, tmp, sizeof(out->timezone));

    nvs_close(h);
    ESP_LOGI(TAG, "Loaded: ssid=%s host=%s name=%s", out->ssid, out->hostname, out->device_name);
}

esp_err_t wifi_manager_save(const wifi_config_nvs_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, "ssid",     cfg->ssid);
    nvs_set_str(h, "pass",     cfg->pass);
    nvs_set_str(h, "ssid2",    cfg->ssid2);
    nvs_set_str(h, "pass2",    cfg->pass2);
    nvs_set_str(h, "hostname", cfg->hostname);
    nvs_set_str(h, "dname",    cfg->device_name);
    nvs_set_str(h, "tz",       cfg->timezone);

    err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi config saved to NVS");
    return err;
}

esp_err_t wifi_manager_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "WiFi NVS namespace erased");
    return err;
}

void wifi_manager_set_mode(wifi_op_mode_t mode)
{
    s_mode = mode;
}

wifi_op_mode_t wifi_manager_get_mode(void)
{
    return s_mode;
}

void wifi_manager_get_ip(char *buf, size_t len)
{
    strlcpy(buf, s_ip, len);
}

void wifi_manager_set_ip(const char *ip_str)
{
    strlcpy(s_ip, ip_str, sizeof(s_ip));
}
