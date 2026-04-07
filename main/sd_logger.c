// ============================================================
//  sd_logger.c — SD card + RAM ring-buffer sensor logging
// ============================================================

#include "sd_logger.h"
#include "sensor_manager.h"
#include "relay.h"
#include "config.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <errno.h>

static const char *TAG = "sd_logger";

// ── Ring-buffer entry ────────────────────────────────────────
typedef struct {
    char  ts[20];           // "YYYY-MM-DDTHH:MM:SS"
    float temp_c;
    float hum_pct;
    int   co2_ppm;
    bool  heater, humidifier, fan, pfan, light;
    float panel_temp_c;
    bool  panel_valid;
} sd_log_entry_t;

static sd_log_entry_t    s_ring[SD_MAX_RECENT];
static int               s_head  = 0;   // next write slot
static int               s_count = 0;   // filled entries (0..SD_MAX_RECENT)
static SemaphoreHandle_t s_mutex = NULL;

static bool              s_mounted     = false;
static bool              s_spi_inited  = false;   // true once SPI2 bus is claimed
static sdmmc_card_t     *s_card        = NULL;
static time_t            s_last_log_t  = 0;
static char              s_mount_err[64] = "not init";

// ── Public: is SD card mounted? / last error string ────────
bool        sd_logger_is_mounted(void)    { return s_mounted; }
const char *sd_logger_mount_error(void)   { return s_mount_err; }
int         sd_logger_recent_count(void)  { return s_count; }
time_t      sd_logger_last_log_time(void) { return s_last_log_t; }

// ── Mount SD card (SPI mode) ─────────────────────────────────
esp_err_t sd_logger_init(void)
{
    s_mutex = xSemaphoreCreateMutex();

    // SPI bus: CLK=GPIO39  MOSI=GPIO40(SD_DI)  MISO=GPIO38(SD_DO)
    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SD_MOSI,
        .miso_io_num     = PIN_SD_MISO,
        .sclk_io_num     = PIN_SD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        snprintf(s_mount_err, sizeof(s_mount_err), "spi_init:%s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "SPI bus init failed: %s", s_mount_err);
        return ret;
    }
    s_spi_inited = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs   = PIN_SD_CS;
    slot.host_id   = SPI2_HOST;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 4096,  // 16K caused DMA OOM on ESP32-S3
    };

    // Allow card time to power up / recover from a CPU reset.
    // After a watchdog or panic reset the SD SPI state machine can take ~800 ms
    // to become responsive again, so use a longer delay and more retries.
    vTaskDelay(pdMS_TO_TICKS(500));

    for (int attempt = 1; attempt <= 5; attempt++) {
        ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "SD mount attempt %d/5 failed: %s", attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (ret != ESP_OK) {
        snprintf(s_mount_err, sizeof(s_mount_err), "%s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "SD mount failed (%s) — RAM-only logging", s_mount_err);
        // Leave SPI bus initialised — remount will reuse it without re-init.
        return ret;
    }
    snprintf(s_mount_err, sizeof(s_mount_err), "ok");

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);

    // Create LOGS dir (ignore EEXIST)
    mkdir(SD_LOG_DIR, 0775);
    ESP_LOGI(TAG, "SD ready: " SD_LOG_DIR);
    return ESP_OK;
}

// ── Append one CSV line to today's file (or boot_log.csv before NTP) ──────
static void append_to_sd(const sd_log_entry_t *e, const char *csv)
{
    // Ensure the logs directory exists (idempotent — harmless if already present)
    mkdir(SD_LOG_DIR, 0775);

    char fname[80];
    if (strncmp(e->ts, "BOOT", 4) == 0)
        snprintf(fname, sizeof(fname), SD_LOG_DIR "/boot_log.csv");
    else
        snprintf(fname, sizeof(fname), SD_LOG_DIR "/%.10s.csv", e->ts); // date part only

    bool new_file = false;
    struct stat st;
    if (stat(fname, &st) != 0) new_file = true;

    FILE *f = fopen(fname, "a");
    if (!f) {
        int logs_errno = errno;
        snprintf(s_mount_err, sizeof(s_mount_err), "fail:e%d logs", logs_errno);
        ESP_LOGW(TAG, "fopen(%s) errno=%d", fname, logs_errno);
        // Fallback: write to SD root (diagnoses whether the LOGS subdir is the issue)
        char fname_root[80];
        if (strncmp(e->ts, "BOOT", 4) == 0)
            snprintf(fname_root, sizeof(fname_root), SD_MOUNT_POINT "/boot_log.csv");
        else
            snprintf(fname_root, sizeof(fname_root), SD_MOUNT_POINT "/%.10s.csv", e->ts);
        new_file = (stat(fname_root, &st) != 0);
        f = fopen(fname_root, "a");
        if (f) {
            snprintf(fname, sizeof(fname), "%s", fname_root);
        }
    }
    if (!f) {
        snprintf(s_mount_err, sizeof(s_mount_err), "fail:e%d root", errno);
        ESP_LOGW(TAG, "fopen failed for all paths, last errno=%d", errno);
        return;
    }
    snprintf(s_mount_err, sizeof(s_mount_err), "ok");

    if (new_file)
        fputs("time,temp_c,hum_pct,co2_ppm,heater,humidifier,fan,pfan,light,panel_temp_c\n", f);
    fputs(csv, f);
    fputc('\n', f);
    fclose(f);
}

// ── Log one entry (call from control_task each cycle) ────────
void sd_logger_log_cycle(void)
{
    time_t now = time(NULL);
    bool ntp_synced = (now >= 1000000000L);
    if (now - s_last_log_t < 60) return;    // one entry per minute

    ESP_LOGI(TAG, "log_cycle: ntp=%d now=%ld last=%ld count=%d mounted=%d",
             (int)ntp_synced, (long)now, (long)s_last_log_t,
             s_count, (int)s_mounted);

    sensor_data_t sd;
    sensor_manager_get(&sd);

    relay_status_t rs[RELAY_COUNT];
    relay_get_all_status(rs);

    sd_log_entry_t e = {0};
    if (ntp_synced) {
        struct tm tms;
        localtime_r(&now, &tms);
        strftime(e.ts, sizeof(e.ts), "%Y-%m-%dT%H:%M:%S", &tms);
    } else {
        snprintf(e.ts, sizeof(e.ts), "BOOT+%05lds", (long)now);  // uptime before NTP
    }
    e.temp_c       = sd.temp_rh_valid  ? sd.temperature_c : 0.0f;
    e.hum_pct      = sd.temp_rh_valid  ? sd.humidity_pct  : 0.0f;
    e.co2_ppm      = sd.co2_valid      ? (int)sd.co2_ppm  : 0;
    e.heater       = rs[RELAY_HEATER].state;
    e.humidifier   = rs[RELAY_HUMIDIFIER].state;
    e.fan          = rs[RELAY_FAN].state;
    e.pfan         = rs[RELAY_PANEL_FAN].state;
    e.light        = rs[RELAY_LIGHT].state;
    e.panel_temp_c = sd.panel_temp_c;
    e.panel_valid  = sd.panel_temp_valid;

    char csv[160];
    snprintf(csv, sizeof(csv), "%s,%.1f,%.1f,%d,%d,%d,%d,%d,%d,%.1f",
             e.ts, e.temp_c, e.hum_pct, e.co2_ppm,
             (int)e.heater, (int)e.humidifier, (int)e.fan,
             (int)e.pfan, (int)e.light,
             e.panel_valid ? e.panel_temp_c : 0.0f);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_ring[s_head] = e;
    s_head  = (s_head + 1) % SD_MAX_RECENT;
    if (s_count < SD_MAX_RECENT) s_count++;
    int new_count = s_count;
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "ring buf entry %d: ts=%s  temp=%.1f  hum=%.1f  co2=%d",
             new_count, e.ts, e.temp_c, e.hum_pct, e.co2_ppm);

    if (s_mounted) append_to_sd(&e, csv);
    s_last_log_t = now;
}
size_t sd_logger_recent_json(char *out, size_t out_size)
{
    if (!out || out_size < 4) return 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    size_t pos = (size_t)snprintf(out, out_size, "[");
    bool   first = true;
    int    start = (s_head - s_count + SD_MAX_RECENT) % SD_MAX_RECENT;

    for (int i = 0; i < s_count; i++) {
        if (pos + 220 >= out_size) break;   // safety margin
        const sd_log_entry_t *e = &s_ring[(start + i) % SD_MAX_RECENT];
        pos += (size_t)snprintf(out + pos, out_size - pos,
            "%s{\"ts\":\"%s\",\"temp\":%.1f,\"hum\":%.1f,\"co2\":%d,"
            "\"heater\":%s,\"humidifier\":%s,\"fan\":%s,\"pfan\":%s,\"light\":%s,\"ptmp\":%.1f}",
            first ? "" : ",",
            e->ts, e->temp_c, e->hum_pct, e->co2_ppm,
            e->heater      ? "true" : "false",
            e->humidifier  ? "true" : "false",
            e->fan         ? "true" : "false",
            e->pfan        ? "true" : "false",
            e->light       ? "true" : "false",
            e->panel_valid ? e->panel_temp_c : 0.0f);
        first = false;
    }

    pos += (size_t)snprintf(out + pos, out_size - pos, "]");
    xSemaphoreGive(s_mutex);
    return pos;
}

// ── JSON: list ALL files on SD card (recursive, for debugging) ─────────────
size_t sd_logger_ls_json(char *out, size_t out_size)
{
    if (!out || out_size < 4) return 0;
    size_t pos = (size_t)snprintf(out, out_size, "[");
    bool first = true;

    if (s_mounted) {
        // Walk two levels: root and one subdir
        const char *dirs[] = { SD_MOUNT_POINT, SD_LOG_DIR };
        for (int d = 0; d < 2; d++) {
            DIR *dir = opendir(dirs[d]);
            if (!dir) continue;
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char fpath[300];
                snprintf(fpath, sizeof(fpath), "%s/%s", dirs[d], ent->d_name);
                struct stat st;
                long sz = (stat(fpath, &st) == 0) ? (long)st.st_size : -1;
                if (pos + 320 >= out_size) break;
                pos += (size_t)snprintf(out + pos, out_size - pos,
                    "%s{\"path\":\"%s\",\"size\":%ld}",
                    first ? "" : ",", fpath, sz);
                first = false;
            }
            closedir(dir);
        }
    }
    pos += (size_t)snprintf(out + pos, out_size - pos, "]");
    return pos;
}

// ── JSON: list of CSV files on SD card ───────────────────────
size_t sd_logger_files_json(char *out, size_t out_size)
{
    if (!out || out_size < 4) return 0;

    size_t pos = (size_t)snprintf(out, out_size, "[");

    if (s_mounted) {
        DIR *dir = opendir(SD_LOG_DIR);
        if (dir) {
            bool first = true;
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (ent->d_type != DT_REG) continue;
                size_t nl = strlen(ent->d_name);
                if (nl < 4 || strcasecmp(ent->d_name + nl - 4, ".csv") != 0) continue;
                if (pos + 80 >= out_size) break;
                char fpath[300];    // NAME_MAX (255) + SD_LOG_DIR prefix + null
                snprintf(fpath, sizeof(fpath), SD_LOG_DIR "/%s", ent->d_name);
                struct stat st;
                long sz = (stat(fpath, &st) == 0) ? (long)st.st_size : 0;
                pos += (size_t)snprintf(out + pos, out_size - pos,
                    "%s{\"name\":\"%s\",\"size\":%ld}",
                    first ? "" : ",", ent->d_name, sz);
                first = false;
            }
            closedir(dir);
        }
    }

    pos += (size_t)snprintf(out + pos, out_size - pos, "]");
    return pos;
}

// ── Remount SD card (callable after a failed boot or CPU reset) ──────────────
// Properly unmounts (if mounted), frees the SPI bus, then re-initialises
// with the same relaxed timing as sd_logger_init().
esp_err_t sd_logger_remount(void)
{
    if (!s_mutex) return ESP_FAIL;

    // Unmount filesystem if it was previously mounted
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
        s_card    = NULL;
        s_mounted = false;
        // esp_vfs_fat_sdcard_unmount releases the SDSPI device but NOT the SPI bus
    }

    // Free + re-init SPI bus only if it was previously initialised.
    // Skipping re-init when bus is already claimed avoids ESP_ERR_INVALID_ARG.
    if (s_spi_inited) {
        spi_bus_free(SPI2_HOST);
        s_spi_inited = false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    snprintf(s_mount_err, sizeof(s_mount_err), "remounting");

    spi_bus_config_t bus = {
        .mosi_io_num     = PIN_SD_MOSI,
        .miso_io_num     = PIN_SD_MISO,
        .sclk_io_num     = PIN_SD_CLK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        snprintf(s_mount_err, sizeof(s_mount_err), "spi:%s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "SD remount SPI init failed: %s", s_mount_err);
        return ret;
    }
    s_spi_inited = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = SPI2_HOST;
    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 4096,  // 16K caused DMA OOM on ESP32-S3
    };

    for (int attempt = 1; attempt <= 5; attempt++) {
        ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "SD remount attempt %d/5: %s", attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (ret != ESP_OK) {
        snprintf(s_mount_err, sizeof(s_mount_err), "%s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "SD remount failed: %s", s_mount_err);
        return ret;
    }

    s_mounted = true;
    mkdir(SD_LOG_DIR, 0775);
    snprintf(s_mount_err, sizeof(s_mount_err), "ok");
    ESP_LOGI(TAG, "SD remounted OK");
    return ESP_OK;
}
