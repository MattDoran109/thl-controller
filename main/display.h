#pragma once

// ============================================================
//  display.h — LVGL touchscreen UI for WT32-SC01 PLUS
//  Initialises the ST7796 LCD and FT5x06 touch controller,
//  then builds a live dashboard screen using LVGL.
// ============================================================

#include "esp_err.h"

// Initialise display hardware + LVGL
esp_err_t display_init(void);

// Call from a dedicated LVGL task (or in the LVGL timer handler)
// to update on-screen values from the latest sensor/relay data.
void display_update(void);

// Update the title bar device name label (call after saving a new name to NVS)
void display_set_device_name(const char *name);
