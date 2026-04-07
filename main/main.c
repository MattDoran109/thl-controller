// ============================================================
//  main.c — THL Controller
//  WT32-SC01 PLUS (ESP32-S3)
//
//  Tasks:
//    sensor_task     — reads sensors every SENSOR_READ_INTERVAL_MS
//    control_task    — runs control cycle after each sensor read
//    display_task    — updates LVGL UI every 500ms
//    (web server runs on its own ESP-IDF httpd threads)
// ============================================================

#include "config.h"
#include "relay.h"
#include "light_pwm.h"
#include "sensor_manager.h"
#include "controller.h"
#include "web_server.h"
#include "display.h"
#include "sd_logger.h"
#include "wifi_manager.h"
#include "notifier.h"
#include "alert_manager.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <time.h>

static const char *TAG = "main";

// ============================================================
// WiFi init — two-stage STA (primary → fallback) then SoftAP
// ============================================================
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_count++;
        if (s_retry_count <= WIFI_STA_MAX_RETRIES) {
            // Fast retries during initial connect / brief dropout
            ESP_LOGW(TAG, "WiFi retry %d/%d", s_retry_count, WIFI_STA_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            // Retry budget exhausted during startup — signal failure so
            // wifi_try_connect() can fall back to the secondary SSID / SoftAP.
            // During normal operation (after initial connect) the retry count is
            // reset to 0 on every successful IP_EVENT_STA_GOT_IP, so this branch
            // is only reached when we lose the AP entirely for an extended period.
            // In that case keep retrying with a 30 s backoff so the device
            // recovers automatically when the router comes back, rather than
            // staying off the air permanently.
            ESP_LOGW(TAG, "WiFi retry %d — backing off 30 s", s_retry_count);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            vTaskDelay(pdMS_TO_TICKS(30000));
            ESP_LOGI(TAG, "WiFi backoff done — reconnecting");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        char ip_str[16];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);
        wifi_manager_set_ip(ip_str);
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// Try connecting to one SSID; returns true on success.
static bool wifi_try_connect(esp_netif_t *sta, const char *ssid, const char *pass)
{
    if (!ssid || ssid[0] == '\0') return false;
    ESP_LOGI(TAG, "Trying WiFi SSID: %s", ssid);
    s_retry_count = 0;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid,     ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode =
        (pass && pass[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));  // let stack process disconnect before reconfiguring
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_STA_MAX_RETRIES * 3000 + 2000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

static void start_ntp(const char *tz)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();
    for (int i = 0; i < 20 && sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED; i++)
        vTaskDelay(pdMS_TO_TICKS(500));
    setenv("TZ", tz, 1);
    tzset();
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED)
        ESP_LOGI(TAG, "NTP synced — tz: %s", tz);
    else
        ESP_LOGW(TAG, "NTP timeout — schedule may be unreliable");
}

static void start_softap(const char *hostname)
{
    ESP_LOGW(TAG, "Starting SoftAP: SSID=%s", WIFI_AP_SSID);
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid            = WIFI_AP_SSID,
            .ssid_len        = strlen(WIFI_AP_SSID),
            .channel         = WIFI_AP_CHANNEL,
            .password        = WIFI_AP_PASS,
            .max_connection  = 4,
            .authmode        = (strlen(WIFI_AP_PASS) >= 8)
                                   ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();
    wifi_manager_set_ip("192.168.4.1");
    wifi_manager_set_mode(WM_MODE_SOFTAP);
    ESP_LOGI(TAG, "SoftAP up — connect to '%s', browse http://192.168.4.1", WIFI_AP_SSID);
}

static esp_err_t wifi_init(void)
{
    wifi_config_nvs_t wcfg;
    wifi_manager_load(&wcfg);

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, wcfg.hostname);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,    &wifi_event_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);  // disable modem sleep — this is a hosted server, not a battery device

    // If no credentials stored at all → go straight to SoftAP for first-run setup
    if (wcfg.ssid[0] == '\0') {
        esp_wifi_stop();
        start_softap(wcfg.hostname);
        return ESP_FAIL;  // web server still starts in AP mode
    }

    // Try primary network
    if (wifi_try_connect(sta, wcfg.ssid, wcfg.pass)) {
        wifi_manager_set_mode(WM_MODE_STA_PRIMARY);
        start_ntp(wcfg.timezone);
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Primary WiFi failed");

    // Try fallback network
    if (wcfg.ssid2[0] != '\0' && wifi_try_connect(sta, wcfg.ssid2, wcfg.pass2)) {
        wifi_manager_set_mode(WM_MODE_STA_FALLBACK);
        start_ntp(wcfg.timezone);
        return ESP_OK;
    }
    if (wcfg.ssid2[0] != '\0')
        ESP_LOGW(TAG, "Fallback WiFi also failed");

    // Both failed — start SoftAP so user can fix credentials
    esp_wifi_stop();
    start_softap(wcfg.hostname);
    return ESP_FAIL;  // signals caller to still start web server
}

static void mdns_init_service(void)
{
    wifi_config_nvs_t wcfg;
    wifi_manager_load(&wcfg);
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(wcfg.hostname);
    mdns_instance_name_set(wcfg.device_name);
    mdns_service_add(NULL, "_http", "_tcp", WEB_SERVER_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local (%s)", wcfg.hostname, wcfg.device_name);
}

// ============================================================
// FreeRTOS tasks
// ============================================================
// Exposed to web_server for diagnostic status
volatile uint32_t g_ctrl_cycles = 0;

static void sensor_task(void *pv)
{
    ESP_LOGI(TAG, "Sensor task started");
    uint32_t cycle = 0;
    while (1) {
        cycle++;
        sensor_manager_read();

        // Guard SD writes against DMA heap exhaustion.
        // After many hours the internal DMA heap can fragment to the point where
        // the SPI-master driver can't allocate its 512-byte per-transaction
        // DMA buffer, causing a LoadProhibited crash.  Skipping the SD write
        // when headroom is critically low prevents the crash and gives the heap
        // time to recover; the missing sample is non-critical.
        size_t dma_free = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (dma_free < 4096) {
            ESP_LOGW(TAG, "DMA heap critically low (%u B largest block) — skipping SD write this cycle", (unsigned)dma_free);
        } else {
            sd_logger_log_cycle();
        }

        // Log DMA heap every ~5 minutes (10 × 30 s cycles) so we can watch for drift
        if (cycle % 10 == 0) {
            size_t dma_total = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
            ESP_LOGI(TAG, "DMA heap: largest_block=%u B  total_free=%u B", (unsigned)dma_free, (unsigned)dma_total);
        }

        alert_manager_check();
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
    }
}

static void control_task(void *pv)
{
    ESP_LOGI(TAG, "Control task entry");  // confirm task was scheduled
    // Brief delay so the first sensor_task cycle has a chance to complete
    // before we start issuing relay commands.  3 s is plenty.
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Control task started");
    while (1) {
        g_ctrl_cycles++;   // increment BEFORE controller so we can detect hangs
        ESP_LOGI(TAG, "Control cycle %lu", (unsigned long)g_ctrl_cycles);
        controller_run_cycle();
        vTaskDelay(pdMS_TO_TICKS(SENSOR_READ_INTERVAL_MS));
    }
}

// display updates are driven by an LVGL timer (see display_init),
// so no separate display_task is needed.

// ============================================================
// app_main
// ============================================================
void app_main(void)
{
    ESP_LOGI(TAG, "THL Controller starting...");

    // NVS init (required for WiFi + setpoint persistence)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS partition");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    notifier_init();
    alert_manager_init();

    // Core subsystem init
    // sensor_manager_init() must run first — it creates the shared I2C bus
    // that relay_init() uses to initialise the MCP23017.
    ESP_ERROR_CHECK(sensor_manager_init());
    ret = relay_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Relay init failed (MCP23017 not responding): %s — outputs disabled", esp_err_to_name(ret));
    ret = light_pwm_init();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Light PWM init failed: %s", esp_err_to_name(ret));
    ESP_ERROR_CHECK(controller_init());
    ESP_ERROR_CHECK(display_init());

    // WiFi — non-fatal; controller works offline.
    // Web server starts in both STA and SoftAP modes.
    wifi_init();
    if (wifi_manager_get_mode() != WM_MODE_OFFLINE) {
        // Only advertise mDNS when on a real network (not SoftAP)
        if (wifi_manager_get_mode() != WM_MODE_SOFTAP)
            mdns_init_service();
        web_server_start();
    }

    // SD card logging — initialised AFTER WiFi so the card gets maximum
    // power-on stabilisation time (~15-30 s on a typical cold boot vs ~1 s
    // if we init before wifi_init).  This also means the web server is already
    // responding before the 5-retry SD init loop runs.
    sd_logger_init();

    // Start background tasks
    // Display updates are driven by an LVGL timer (registered in display_init),
    // so no separate display task is needed.
    ESP_LOGI(TAG, "Free internal RAM before tasks: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    BaseType_t r;
    r = xTaskCreatePinnedToCore(sensor_task,  "sensors", 8192, NULL, 5, NULL, 1);
    if (r != pdPASS) ESP_LOGE(TAG, "sensor_task create FAILED");
    ESP_LOGI(TAG, "Free internal RAM after sensor_task: %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    r = xTaskCreatePinnedToCore(control_task, "control", 6144, NULL, 4, NULL, 1);
    if (r != pdPASS) ESP_LOGE(TAG, "control_task create FAILED");

    ESP_LOGI(TAG, "All tasks started");
}
