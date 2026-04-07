// ============================================================
//  notifier.c — ntfy.sh push notification
// ============================================================

#include "notifier.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "mbedtls/platform.h"
#include <string.h>
#include <stdio.h>

#define TAG        "notifier"
#define NVS_NS     "notifier"
#define NVS_KEY    "topic"

#define NTFY_TITLE_MAX   64
#define NTFY_MSG_MAX     128
#define NTFY_BASE_URL    "https://ntfy.sh/"

typedef struct {
    char title[NTFY_TITLE_MAX];
    char message[NTFY_MSG_MAX];
} notifier_msg_t;

static char s_topic[NOTIFIER_TOPIC_MAX] = {0};

static void *s_mbedtls_calloc_psram(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// ── one-shot task: POSTs to ntfy.sh then deletes itself ──────
static void notifier_send_task(void *arg)
{
    notifier_msg_t *msg = (notifier_msg_t *)arg;

    // Route mbedTLS allocations to PSRAM — internal RAM is too fragmented.
    mbedtls_platform_set_calloc_free(s_mbedtls_calloc_psram, heap_caps_free);

    char url[128];
    snprintf(url, sizeof(url), NTFY_BASE_URL "%s", s_topic);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "text/plain");
        if (msg->title[0])
            esp_http_client_set_header(client, "Title", msg->title);
        esp_http_client_set_post_field(client, msg->message, strlen(msg->message));

        esp_err_t err = esp_http_client_perform(client);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "ntfy POST failed: %s", esp_err_to_name(err));
        else
            ESP_LOGI(TAG, "ntfy POST status: %d", esp_http_client_get_status_code(client));

        esp_http_client_cleanup(client);
    } else {
        ESP_LOGE(TAG, "http_client_init failed");
    }

    free(msg);
    vTaskDelete(NULL);
}

// ── synchronous send — called directly from the test handler ─
static int do_ntfy_post(const char *title, const char *message)
{
    char url[128];
    snprintf(url, sizeof(url), NTFY_BASE_URL "%s", s_topic);

    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { ESP_LOGE(TAG, "http_client_init failed"); return -1; }

    esp_http_client_set_header(client, "Content-Type", "text/plain");
    if (title && title[0])
        esp_http_client_set_header(client, "Title", title);
    esp_http_client_set_post_field(client, message, strlen(message));

    int status = -1;
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "ntfy POST failed: %s", esp_err_to_name(err));
    else
        status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);
    return status;
}

int notifier_send_sync(const char *title, const char *message)
{
    if (!title || !message || s_topic[0] == '\0') {
        ESP_LOGW(TAG, "send_sync: no topic or bad args");
        return -1;
    }
    int status = do_ntfy_post(title, message);
    ESP_LOGI(TAG, "send_sync result: %d", status);
    return status;
}

void notifier_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_topic);
        if (nvs_get_str(h, NVS_KEY, s_topic, &len) != ESP_OK)
            s_topic[0] = '\0';
        nvs_close(h);
    }
    ESP_LOGI(TAG, "notifier ready, topic='%s'", s_topic);
}

void notifier_get_topic(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strlcpy(buf, s_topic, len);
}

void notifier_set_topic(const char *topic)
{
    if (!topic) return;
    strlcpy(s_topic, topic, sizeof(s_topic));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY, s_topic);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "topic saved: '%s'", s_topic);
}

void notifier_send(const char *title, const char *message)
{
    if (!title || !message || s_topic[0] == '\0') {
        if (s_topic[0] == '\0') ESP_LOGW(TAG, "no topic configured, dropping notification");
        return;
    }

    notifier_msg_t *msg = malloc(sizeof(notifier_msg_t));
    if (!msg) { ESP_LOGE(TAG, "OOM, dropping notification"); return; }
    strlcpy(msg->title,   title,   sizeof(msg->title));
    strlcpy(msg->message, message, sizeof(msg->message));

    // Allocate task stack from PSRAM — internal RAM too fragmented for contiguous block.
    if (xTaskCreateWithCaps(notifier_send_task, "ntfy_send", 4096, msg, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "send task create failed (free IRAM:%u SPIRAM:%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        free(msg);
    }
}
