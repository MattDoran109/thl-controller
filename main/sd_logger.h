#pragma once
// ============================================================
//  sd_logger.h — SD card + RAM ring-buffer sensor logging
//  Daily CSV files: /sdcard/logs/YYYY-MM-DD.csv
//  RAM ring: last SD_MAX_RECENT entries, survives SD absence.
// ============================================================

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

#define SD_MOUNT_POINT  "/sdcard"
#define SD_LOG_DIR      "/sdcard/LOGS"
#define SD_MAX_RECENT   360     // entries kept in RAM ring buffer (~1 hour at 10 s/entry)

// Mount SD card and create log directory.
// Non-fatal: if no card is present, RAM-only logging continues.
esp_err_t sd_logger_init(void);

// Call once per controller cycle.  Logs at most once per minute.
void      sd_logger_log_cycle(void);

// Fill 'out' with a JSON array of recent entries (oldest first).
// Returns bytes written (excluding null terminator).
size_t    sd_logger_recent_json(char *out, size_t out_size);

// Fill 'out' with a JSON array of {name, size} objects for log files on SD.
size_t    sd_logger_files_json(char *out, size_t out_size);

bool        sd_logger_is_mounted(void);
const char *sd_logger_mount_error(void);
int         sd_logger_recent_count(void);  // number of entries in ring buffer
time_t      sd_logger_last_log_time(void); // epoch time of last logged entry (0 = none)
size_t      sd_logger_ls_json(char *out, size_t out_size);  // list all files on card
esp_err_t   sd_logger_remount(void);  // unmount + re-mount (e.g. after CPU reset)
