// ============================================================
//  display.c — WT32-SC01 PLUS display & touch initialisation
//              + LVGL dashboard UI
//
//  Hardware:
//    LCD:   ST7796  320×480  16-bit Intel 8080 parallel bus
//    Touch: FT5x06  I2C (separate I2C bus from sensors)
//
//  Requires components (add to idf_component.yml):
//    espressif/esp_lvgl_port  >= 2.3.0
//    espressif/esp_lcd_touch_ft5x06 >= 1.0.3
// ============================================================

#include "display.h"
#include "config.h"
#include "sensor_manager.h"
#include "relay.h"
#include "controller.h"
#include "wifi_manager.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "display";

// ---- Handles -----------------------------------------------
static esp_lcd_panel_io_handle_t  s_io_handle    = NULL;
static esp_lcd_panel_handle_t     s_panel        = NULL;
static esp_lcd_touch_handle_t     s_touch        = NULL;
static lv_display_t              *s_lv_display   = NULL;

// ---- Home screen widgets -----------------------------------
static lv_obj_t *s_lbl_temp       = NULL;   // numeric value only, e.g. "24.5"
static lv_obj_t *s_lbl_hum        = NULL;   // numeric value only, e.g. "65"
static lv_obj_t *s_lbl_co2        = NULL;   // numeric value only, e.g. "800"
static lv_obj_t *s_lbl_level      = NULL;   // water level indicator
static lv_obj_t *s_lbl_status     = NULL;
static lv_obj_t *s_btn_relays[RELAY_COUNT] = {NULL};
static lv_obj_t *s_btn_icons[RELAY_COUNT]  = {NULL};
static lv_obj_t *s_btn_lbl_status[RELAY_COUNT] = {NULL};
static lv_obj_t *s_btn_lbl_name[RELAY_COUNT]   = {NULL};  // tracked for ON/OFF colour change

// ---- Title bar label (updated when device name changes) ----
static lv_obj_t *s_lbl_title      = NULL;
static lv_obj_t *s_lbl_time       = NULL;   // HH:MM clock in title bar

// ---- Detail panel widgets ----------------------------------
static lv_obj_t *s_detail         = NULL;   // full-screen overlay (NULL = hidden)
static int       s_detail_id      = -1;     // which relay is being edited

// Editable value labels (updated by +/- buttons)
static lv_obj_t *s_edit_lbl_v1    = NULL;
static lv_obj_t *s_edit_lbl_v2    = NULL;
static lv_obj_t *s_edit_lbl_v3    = NULL;   // light: on hour/off hour labels
static lv_obj_t *s_edit_lbl_v4    = NULL;   // light: on min/off min labels

// Current edit values (read back on save)
static float     s_edit_f1        = 0;
static float     s_edit_f2        = 0;
static int       s_edit_i3        = 0;   // light on_hour or off_hour
static int       s_edit_i4        = 0;   // light on_min or off_min
static int       s_edit_i5        = 0;   // light off_hour
static int       s_edit_i6        = 0;   // light off_min
static int       s_edit_i7        = 0;   // fan sched on_min
static int       s_edit_i8        = 0;   // fan sched period_min

// ============================================================
// LCD hardware init
// ============================================================
static esp_err_t lcd_init(void)
{
    // Backlight
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << PIN_LCD_BK_LIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl);
    gpio_set_level(PIN_LCD_BK_LIGHT, 1);

    // Panel IO (8080 parallel)
    esp_lcd_i80_bus_handle_t bus;
    // WT32-SC01 PLUS uses an 8-bit MCU 8080 bus (NOT 16-bit).
    // LCD_BIT_WIDTH = 8, data_gpio_nums only needs 8 entries.
    esp_lcd_i80_bus_config_t bus_cfg = {
        .dc_gpio_num         = PIN_LCD_DC,
        .wr_gpio_num         = PIN_LCD_WR,
        .clk_src             = LCD_CLK_SRC_DEFAULT,
        .data_gpio_nums      = {
            PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
            PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7,
        },
        .bus_width           = LCD_BIT_WIDTH,   // 8
        .max_transfer_bytes  = LCD_H_RES * 20 * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num         = PIN_LCD_CS,
        .pclk_hz             = 40 * 1000 * 1000,
        .trans_queue_depth   = 4,
        .dc_levels           = {
            .dc_idle_level   = 0,
            .dc_cmd_level    = 0,
            .dc_dummy_level  = 0,
            .dc_data_level   = 1,
        },
        .flags               = { .swap_color_bytes = true },  // ST7796 I80 8-bit bus needs MSB first
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(bus, &io_cfg, &s_io_handle));

    // ST7796 panel
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = PIN_LCD_RST,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(s_io_handle, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    // ST7796UI on WT32-SC01 PLUS requires colour inversion
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));  // rotation applied by lvgl_port

    ESP_LOGI(TAG, "LCD initialised (%dx%d)", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}

// ============================================================
// Touch hardware init (FT5x06 on I2C bus 0)
// ============================================================
static esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port      = I2C_NUM_0,
        .sda_io_num    = PIN_TOUCH_SDA,
        .scl_io_num    = PIN_TOUCH_SCL,
        .clk_source    = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    esp_lcd_touch_config_t touch_cfg = {
        .x_max       = LCD_V_RES,   // 320 — raw portrait sensor width
        .y_max       = LCD_H_RES,   // 480 — raw portrait sensor height
        .rst_gpio_num = PIN_TOUCH_RST,
        .int_gpio_num = GPIO_NUM_NC,
        .levels      = { .reset = 0, .interrupt = 0 },
        // 90° CCW: mirror_x folds raw_x (0→320), then swap_xy maps
        // portrait→landscape: final_x=raw_y (0→480), final_y=320-raw_x (0→320)
        .flags       = { .swap_xy = 1, .mirror_x = 0, .mirror_y = 1 },
    };

    esp_lcd_panel_io_handle_t tp_io;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = {
        .dev_addr    = TOUCH_I2C_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 0,
        .flags.disable_control_phase = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c_v2(i2c_bus, &tp_io_cfg, &tp_io));
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io, &touch_cfg, &s_touch));

    ESP_LOGI(TAG, "Touch initialised");
    return ESP_OK;
}

// ============================================================
// Detail panel helpers
// ============================================================

// Adjust a float edit value and update label
static void float_adj(float *val, lv_obj_t *lbl, float step, float lo, float hi, int decimals)
{
    *val += step;
    if (*val < lo) *val = lo;
    if (*val > hi) *val = hi;
    char buf[16];
    if (decimals == 1) snprintf(buf, sizeof(buf), "%.1f", *val);
    else               snprintf(buf, sizeof(buf), "%.0f", *val);
    lv_label_set_text(lbl, buf);
}

static void int_adj(int *val, lv_obj_t *lbl, int step, int lo, int hi, const char *fmt)
{
    *val += step;
    if (*val < lo) *val = lo;
    if (*val > hi) *val = hi;
    char buf[16];
    snprintf(buf, sizeof(buf), fmt, *val);
    lv_label_set_text(lbl, buf);
}

// Event callbacks for +/- buttons
static void cb_v1_dec(lv_event_t *e) {
    float step = *(float *)lv_event_get_user_data(e);
    float lo = (s_detail_id == RELAY_FAN) ? 400
             : (s_detail_id == RELAY_HUMIDIFIER) ? 30 : 15;
    float hi = (s_detail_id == RELAY_FAN) ? 5000
             : (s_detail_id == RELAY_HUMIDIFIER) ? 90 : 40;
    float_adj(&s_edit_f1, s_edit_lbl_v1, -step, lo, hi, (step < 1.0f) ? 1 : 0);
}
static void cb_v1_inc(lv_event_t *e) {
    float step = *(float *)lv_event_get_user_data(e);
    float lo = (s_detail_id == RELAY_FAN) ? 400
             : (s_detail_id == RELAY_HUMIDIFIER) ? 30 : 15;
    float hi = (s_detail_id == RELAY_FAN) ? 5000
             : (s_detail_id == RELAY_HUMIDIFIER) ? 90 : 40;
    float_adj(&s_edit_f1, s_edit_lbl_v1,  step, lo, hi, (step < 1.0f) ? 1 : 0);
}
static void cb_v2_dec(lv_event_t *e) {
    float step = *(float *)lv_event_get_user_data(e);
    float_adj(&s_edit_f2, s_edit_lbl_v2, -step, 0, 20, (step < 1.0f) ? 1 : 0);
}
static void cb_v2_inc(lv_event_t *e) {
    float step = *(float *)lv_event_get_user_data(e);
    float_adj(&s_edit_f2, s_edit_lbl_v2,  step, 0, 20, (step < 1.0f) ? 1 : 0);
}
static void cb_v3_dec(lv_event_t *e) {  // ON hour (0) / OFF hour (1) / fan on_min (2)
    int range = (intptr_t)lv_event_get_user_data(e);
    if (range == 2) { int_adj(&s_edit_i7, s_edit_lbl_v3, -1,  1, 60, "%d"); return; }
    int *val  = (range == 0) ? &s_edit_i3 : &s_edit_i5;
    lv_obj_t *lbl = (range == 0) ? s_edit_lbl_v1 : s_edit_lbl_v3;
    int_adj(val, lbl, -1, 0, 23, "%02d");
}
static void cb_v3_inc(lv_event_t *e) {
    int range = (intptr_t)lv_event_get_user_data(e);
    if (range == 2) { int_adj(&s_edit_i7, s_edit_lbl_v3, +1,  1, 60, "%d"); return; }
    int *val  = (range == 0) ? &s_edit_i3 : &s_edit_i5;
    lv_obj_t *lbl = (range == 0) ? s_edit_lbl_v1 : s_edit_lbl_v3;
    int_adj(val, lbl, +1, 0, 23, "%02d");
}
static void cb_v4_dec(lv_event_t *e) {  // ON min (0) / OFF min (1) / fan period_min (2)
    int range = (intptr_t)lv_event_get_user_data(e);
    if (range == 2) { int_adj(&s_edit_i8, s_edit_lbl_v4, -5,  5, 240, "%d"); return; }
    int *val  = (range == 0) ? &s_edit_i4 : &s_edit_i6;
    lv_obj_t *lbl = (range == 0) ? s_edit_lbl_v2 : s_edit_lbl_v4;
    int_adj(val, lbl, -5, 0, 55, "%02d");
}
static void cb_v4_inc(lv_event_t *e) {
    int range = (intptr_t)lv_event_get_user_data(e);
    if (range == 2) { int_adj(&s_edit_i8, s_edit_lbl_v4, +5,  5, 240, "%d"); return; }
    int *val  = (range == 0) ? &s_edit_i4 : &s_edit_i6;
    lv_obj_t *lbl = (range == 0) ? s_edit_lbl_v2 : s_edit_lbl_v4;
    int_adj(val, lbl, +5, 0, 55, "%02d");
}

static void open_detail(int idx);       // forward declaration
static void cb_detail_back(lv_event_t *e); // forward declaration

static void cb_detail_save(lv_event_t *e)
{
    controller_setpoints_t sp;
    controller_get_setpoints(&sp);
    switch (s_detail_id) {
        case RELAY_HUMIDIFIER:
            sp.hum_setpoint   = s_edit_f1;
            sp.hum_hysteresis = s_edit_f2;
            break;
        case RELAY_HEATER:
            sp.temp_setpoint   = s_edit_f1;
            sp.temp_hysteresis = s_edit_f2;
            break;
        case RELAY_FAN:
            sp.co2_threshold       = (uint16_t)s_edit_f1;
            sp.co2_hysteresis      = (uint16_t)s_edit_f2;
            sp.fan_sched_on_min    = s_edit_i7;
            sp.fan_sched_period_min= s_edit_i8;
            break;
        case RELAY_LIGHT:
            sp.light_on_hour  = s_edit_i3;
            sp.light_on_min   = s_edit_i4;
            sp.light_off_hour = s_edit_i5;
            sp.light_off_min  = s_edit_i6;
            break;
    }
    controller_set_setpoints(&sp);
    cb_detail_back(NULL);  // return to home screen after save
}

static void cb_detail_back(lv_event_t *e)
{
    if (s_detail) {
        // Use delete_async so the deletion happens outside this event callback.
        // LVGL v9 defers lv_obj_delete() from event callbacks but doesn't always
        // invalidate the uncovered area; explicit screen invalidation ensures the
        // sensor cards beneath are redrawn on the very next timer cycle.
        lv_obj_delete_async(s_detail);
        s_detail    = NULL;
        s_detail_id = -1;
        lv_obj_invalidate(lv_screen_active());
    }
}

// Build a +/- spinner row.  Returns the value label pointer.
static lv_obj_t *make_spinner_row(lv_obj_t *parent,
                                   const char *title_str,
                                   const char *init_val,
                                   int y,
                                   lv_event_cb_t dec_cb, lv_event_cb_t inc_cb,
                                   void *user_data)
{
    // Title label — left side, inline with the controls (montserrat_14 keeps it compact)
    lv_obj_t *ttl = lv_label_create(parent);
    lv_label_set_text(ttl, title_str);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0x6b7280), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_14, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_LEFT, 10, y + 14);  // vertically centred in 44px button

    // Minus button — shifted right of screen centre to leave room for label on left
    lv_obj_t *bm = lv_button_create(parent);
    lv_obj_set_size(bm, 44, 44);
    lv_obj_align(bm, LV_ALIGN_TOP_MID, -25, y);
    lv_obj_set_style_bg_color(bm, lv_color_hex(0x6b7280), 0);
    lv_obj_set_style_radius(bm, 8, 0);
    lv_obj_add_event_cb(bm, dec_cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *ml = lv_label_create(bm);
    lv_label_set_text(ml, LV_SYMBOL_MINUS);
    lv_obj_set_style_text_font(ml, &lv_font_montserrat_20, 0);
    lv_obj_align(ml, LV_ALIGN_CENTER, 0, 0);

    // Value label — between the two buttons
    lv_obj_t *vlbl = lv_label_create(parent);
    lv_label_set_text(vlbl, init_val);
    lv_obj_set_style_text_font(vlbl, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(vlbl, lv_color_hex(0x111827), 0);
    lv_obj_align(vlbl, LV_ALIGN_TOP_MID, 40, y + 4);

    // Plus button
    lv_obj_t *bp = lv_button_create(parent);
    lv_obj_set_size(bp, 44, 44);
    lv_obj_align(bp, LV_ALIGN_TOP_MID, 105, y);
    lv_obj_set_style_bg_color(bp, lv_color_hex(0x6b7280), 0);
    lv_obj_set_style_radius(bp, 8, 0);
    lv_obj_add_event_cb(bp, inc_cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *pl = lv_label_create(bp);
    lv_label_set_text(pl, LV_SYMBOL_PLUS);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_20, 0);
    lv_obj_align(pl, LV_ALIGN_CENTER, 0, 0);

    return vlbl;
}

// Creates a 44×44 +/- button for the light time rows, positioned by absolute coords.
static void make_time_btn(lv_obj_t *parent, int x, int y,
                           const char *symbol, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x6b7280), 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
}

// Open detail panel for relay idx
static void open_detail(int idx)
{
    if (s_detail) {
        lv_obj_delete_async(s_detail);
        s_detail = NULL;
        lv_obj_invalidate(lv_screen_active());
    }
    s_detail_id = idx;

    controller_setpoints_t sp;
    controller_get_setpoints(&sp);

    // Full-screen card over the home screen
    s_detail = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_detail, 480, 320);
    lv_obj_align(s_detail, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_detail, lv_color_hex(0xf0f2f5), 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_radius(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 0, 0);  // no default inset — positions are absolute
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);

    // ── Title bar ─────────────────────────────────────────
    lv_obj_t *bar = lv_obj_create(s_detail);
    lv_obj_set_size(bar, 480, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);

    // Back button
    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 70, 34);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0xc73652), 0);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, cb_detail_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(back_lbl, LV_ALIGN_CENTER, 0, 0);

    // Indexed by relay_id_t value — must cover RELAY_COUNT entries.
    static const char *titles[RELAY_COUNT] = {
        [RELAY_HEATER]     = "Heater",
        [RELAY_HUMIDIFIER] = "Humidifier",
        [RELAY_FAN]        = "Fan / CO2",
        [RELAY_PANEL_FAN]  = "Panel Fan",
        [RELAY_LIGHT]      = "Light",
    };
    lv_obj_t *ttl = lv_label_create(bar);
    lv_label_set_text(ttl, titles[idx]);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(ttl, &lv_font_montserrat_20, 0);
    lv_obj_align(ttl, LV_ALIGN_CENTER, 0, 0);

    // ── Setpoint controls (relay-specific) ────────────────
    static float step1_f = 0.5f, step2_f = 0.5f;
    char init1[16], init2[16];

    if (idx == RELAY_HUMIDIFIER) {
        s_edit_f1 = sp.hum_setpoint;   s_edit_f2 = sp.hum_hysteresis;
        snprintf(init1, sizeof(init1), "%.1f", s_edit_f1);
        snprintf(init2, sizeof(init2), "%.1f", s_edit_f2);
        s_edit_lbl_v1 = make_spinner_row(s_detail, "Target %RH", init1, 108,
                                          cb_v1_dec, cb_v1_inc, &step1_f);
        s_edit_lbl_v2 = make_spinner_row(s_detail, "Hysteresis %", init2, 162,
                                          cb_v2_dec, cb_v2_inc, &step2_f);
    } else if (idx == RELAY_HEATER) {
        s_edit_f1 = sp.temp_setpoint;  s_edit_f2 = sp.temp_hysteresis;
        snprintf(init1, sizeof(init1), "%.1f", s_edit_f1);
        snprintf(init2, sizeof(init2), "%.1f", s_edit_f2);
        s_edit_lbl_v1 = make_spinner_row(s_detail, "Target \xc2\xb0""C", init1, 108,
                                          cb_v1_dec, cb_v1_inc, &step1_f);
        s_edit_lbl_v2 = make_spinner_row(s_detail, "Hysteresis \xc2\xb0""C", init2, 162,
                                          cb_v2_dec, cb_v2_inc, &step2_f);
    } else if (idx == RELAY_FAN) {
        s_edit_f1 = sp.co2_threshold;  s_edit_f2 = sp.co2_hysteresis;
        s_edit_i7 = sp.fan_sched_on_min; s_edit_i8 = sp.fan_sched_period_min;
        static float step_co2 = 50.0f, step_co2h = 50.0f;
        char fan_s1[16], fan_s2[16], fan_s3[16], fan_s4[16];
        snprintf(fan_s1, sizeof(fan_s1), "%.0f", s_edit_f1);
        snprintf(fan_s2, sizeof(fan_s2), "%.0f", s_edit_f2);
        snprintf(fan_s3, sizeof(fan_s3), "%d", s_edit_i7);
        snprintf(fan_s4, sizeof(fan_s4), "%d", s_edit_i8);
        s_edit_lbl_v1 = make_spinner_row(s_detail, "CO2 On (ppm)",  fan_s1, 52,
                                          cb_v1_dec, cb_v1_inc, &step_co2);
        s_edit_lbl_v2 = make_spinner_row(s_detail, "Hyst (ppm)",    fan_s2, 100,
                                          cb_v2_dec, cb_v2_inc, &step_co2h);
        s_edit_lbl_v3 = make_spinner_row(s_detail, "Sched ON (min)", fan_s3, 148,
                                          cb_v3_dec, cb_v3_inc, (void *)(intptr_t)2);
        s_edit_lbl_v4 = make_spinner_row(s_detail, "Period (min)",   fan_s4, 196,
                                          cb_v4_dec, cb_v4_inc, (void *)(intptr_t)2);
    } else { // RELAY_LIGHT — 4 values: on HH, on MM, off HH, off MM
        // Layout: two stacked rows (like Heater) at y=108 and y=162.
        // Each row: [label left] [−][HH][+] : [−][MM][+]
        // x: label=10, minusH=128, HH=178, plusH=222, colon=264, minusM=278, MM=328, plusM=372
        s_edit_i3 = sp.light_on_hour;  s_edit_i4 = sp.light_on_min;
        s_edit_i5 = sp.light_off_hour; s_edit_i6 = sp.light_off_min;

        // ── ON row (y=108) ──────────────────────────────
        lv_obj_t *on_lbl = lv_label_create(s_detail);
        lv_label_set_text(on_lbl, "Light ON");
        lv_obj_set_style_text_color(on_lbl, lv_color_hex(0x6b7280), 0);
        lv_obj_set_style_text_font(on_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(on_lbl, LV_ALIGN_TOP_LEFT, 10, 108 + 14);

        snprintf(init1, sizeof(init1), "%02d", s_edit_i3);
        snprintf(init2, sizeof(init2), "%02d", s_edit_i4);
        make_time_btn(s_detail, 128, 108, LV_SYMBOL_MINUS, cb_v3_dec, (void*)(intptr_t)0);
        s_edit_lbl_v1 = lv_label_create(s_detail);
        lv_label_set_text(s_edit_lbl_v1, init1);
        lv_obj_set_style_text_font(s_edit_lbl_v1, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(s_edit_lbl_v1, lv_color_hex(0x111827), 0);
        lv_obj_align(s_edit_lbl_v1, LV_ALIGN_TOP_LEFT, 178, 108 + 4);
        make_time_btn(s_detail, 222, 108, LV_SYMBOL_PLUS,  cb_v3_inc, (void*)(intptr_t)0);
        lv_obj_t *colon1 = lv_label_create(s_detail);
        lv_label_set_text(colon1, ":");
        lv_obj_set_style_text_font(colon1, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(colon1, lv_color_hex(0x111827), 0);
        lv_obj_align(colon1, LV_ALIGN_TOP_LEFT, 264, 108 + 4);
        make_time_btn(s_detail, 278, 108, LV_SYMBOL_MINUS, cb_v4_dec, (void*)(intptr_t)0);
        s_edit_lbl_v2 = lv_label_create(s_detail);
        lv_label_set_text(s_edit_lbl_v2, init2);
        lv_obj_set_style_text_font(s_edit_lbl_v2, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(s_edit_lbl_v2, lv_color_hex(0x111827), 0);
        lv_obj_align(s_edit_lbl_v2, LV_ALIGN_TOP_LEFT, 328, 108 + 4);
        make_time_btn(s_detail, 372, 108, LV_SYMBOL_PLUS,  cb_v4_inc, (void*)(intptr_t)0);

        // ── OFF row (y=162) ─────────────────────────────
        lv_obj_t *off_lbl = lv_label_create(s_detail);
        lv_label_set_text(off_lbl, "Light OFF");
        lv_obj_set_style_text_color(off_lbl, lv_color_hex(0x6b7280), 0);
        lv_obj_set_style_text_font(off_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(off_lbl, LV_ALIGN_TOP_LEFT, 10, 162 + 14);

        snprintf(init1, sizeof(init1), "%02d", s_edit_i5);
        snprintf(init2, sizeof(init2), "%02d", s_edit_i6);
        make_time_btn(s_detail, 128, 162, LV_SYMBOL_MINUS, cb_v3_dec, (void*)(intptr_t)1);
        s_edit_lbl_v3 = lv_label_create(s_detail);
        lv_label_set_text(s_edit_lbl_v3, init1);
        lv_obj_set_style_text_font(s_edit_lbl_v3, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(s_edit_lbl_v3, lv_color_hex(0x111827), 0);
        lv_obj_align(s_edit_lbl_v3, LV_ALIGN_TOP_LEFT, 178, 162 + 4);
        make_time_btn(s_detail, 222, 162, LV_SYMBOL_PLUS,  cb_v3_inc, (void*)(intptr_t)1);
        lv_obj_t *colon2 = lv_label_create(s_detail);
        lv_label_set_text(colon2, ":");
        lv_obj_set_style_text_font(colon2, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(colon2, lv_color_hex(0x111827), 0);
        lv_obj_align(colon2, LV_ALIGN_TOP_LEFT, 264, 162 + 4);
        make_time_btn(s_detail, 278, 162, LV_SYMBOL_MINUS, cb_v4_dec, (void*)(intptr_t)1);
        s_edit_lbl_v4 = lv_label_create(s_detail);
        lv_label_set_text(s_edit_lbl_v4, init2);
        lv_obj_set_style_text_font(s_edit_lbl_v4, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(s_edit_lbl_v4, lv_color_hex(0x111827), 0);
        lv_obj_align(s_edit_lbl_v4, LV_ALIGN_TOP_LEFT, 328, 162 + 4);
        make_time_btn(s_detail, 372, 162, LV_SYMBOL_PLUS,  cb_v4_inc, (void*)(intptr_t)1);
    }

    // ── Save button ───────────────────────────────────────
    lv_obj_t *bsave = lv_button_create(s_detail);
    lv_obj_set_size(bsave, 460, 48);
    lv_obj_align(bsave, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(bsave, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_radius(bsave, 10, 0);
    lv_obj_set_style_border_width(bsave, 0, 0);
    lv_obj_add_event_cb(bsave, cb_detail_save, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_lbl = lv_label_create(bsave);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(save_lbl, LV_ALIGN_CENTER, 0, 0);
}

// ============================================================
// LVGL UI — Dashboard screen
// ============================================================
static void relay_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    open_detail(idx);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xf0f2f5), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ── Title bar (white, 40px) — device name | clock | status ─
    // White background ensures Montserrat renders cleanly without halo
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_set_size(bar, 480, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    // Bottom border line to separate from card row
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(0xe2e8f0), 0);

    // Device name — left, dark
    s_lbl_title = lv_label_create(bar);
    {
        wifi_config_nvs_t _wcfg;
        wifi_manager_load(&_wcfg);
        lv_label_set_text(s_lbl_title,
                 _wcfg.device_name[0] ? _wcfg.device_name : DEVICE_NAME_DEFAULT);
    }
    lv_obj_set_style_text_color(s_lbl_title, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_text_font(s_lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_title, LV_ALIGN_LEFT_MID, 10, 0);

    // Clock — centre
    s_lbl_time = lv_label_create(bar);
    lv_label_set_text(s_lbl_time, "--:--");
    lv_obj_set_style_text_color(s_lbl_time, lv_color_hex(0x1e293b), 0);
    lv_obj_set_style_text_font(s_lbl_time, &lv_font_montserrat_20, 0);
    lv_obj_align(s_lbl_time, LV_ALIGN_CENTER, 0, 0);

    // Status — right
    s_lbl_status = lv_label_create(bar);
    lv_label_set_text(s_lbl_status, "...");
    lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0x64748b), 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_RIGHT_MID, -10, 0);

    // ── Sensor cards (4: Temp, Humidity, CO2, Water) ──────
    // 4 cards × 113px wide with 8px gaps = 4×113 + 5×8 = 492 > 480.
    // Use 110px cards with 8px gaps: 4×110 + 5×8 = 480 exactly.
    // Cards positioned at x = 8, 126, 244, 362 (gap+card repeating)
    // Card height: 113px.  y=42 (just below title bar).
    // Value label: large number only ("--" / "24.5"), font_28, above centre
    // Unit label:  smaller unit text ("°C" / "%" / "ppm"), font_14, below value
    // Name label:  sensor name, font_14, bottom of card
    static const char *s_names[]  = {"Temperature", "Humidity", "CO2",    "Water"};
    static const char *s_units[]  = {"\xc2\xb0""C",  "%",       "ppm",    ""};
    static const uint32_t s_col[] = {0xd4450a,        0x0077c2,  0x1a7a3c, 0x0077c2};
    lv_obj_t **s_vals[]           = {&s_lbl_temp,     &s_lbl_hum, &s_lbl_co2, &s_lbl_level};

    for (int i = 0; i < 4; i++) {
        const int cx = 8 + i * 118;  // card x origin (same 118px step as before)
        lv_obj_t *card = lv_obj_create(scr);
        lv_obj_set_size(card, 113, 113);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, cx, 42);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0xe2e8f0), 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_set_style_pad_all(card, 0, 0);  // no padding — position labels explicitly

        // Value: just the number, large, top-centre of card
        *s_vals[i] = lv_label_create(card);
        lv_label_set_text(*s_vals[i], (i == 3) ? "OK" : "--");
        lv_obj_set_style_text_font(*s_vals[i], &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(*s_vals[i], lv_color_hex(s_col[i]), 0);
        lv_obj_set_style_text_align(*s_vals[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(*s_vals[i], LV_ALIGN_TOP_MID, 0, 18);

        // Unit: small, immediately below the value
        lv_obj_t *unit = lv_label_create(card);
        lv_label_set_text(unit, s_units[i]);
        lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(unit, lv_color_hex(s_col[i]), 0);
        lv_obj_set_style_text_align(unit, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(unit, LV_ALIGN_TOP_MID, 0, 58);

        // Name: grey label at bottom
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x94a3b8), 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    // ── Relay buttons (tap = open detail panel) ───────────
    // 4 user-facing relays — RELAY_PANEL_FAN is internal only, not shown.
    // Button height: 320 - 42 - 113 - 6 = 159px.  Use 157px for a 2px bottom margin.
    static const relay_id_t b_relays[] = {
        RELAY_HEATER, RELAY_HUMIDIFIER, RELAY_FAN, RELAY_LIGHT
    };
    // Icons chosen to render cleanly at font_28 size
    static const char *b_icons[] = {
        LV_SYMBOL_WARNING,   // heater
        LV_SYMBOL_TINT,      // humidifier
        LV_SYMBOL_LOOP,      // fan
        LV_SYMBOL_EYE_OPEN   // light
    };
    static const char *b_names[] = {"Heater", "Humid", "Fan", "Light"};

    for (int i = 0; i < 4; i++) {
        const int bx = 8 + i * 118;
        lv_obj_t *btn = lv_button_create(scr);
        lv_obj_set_size(btn, 113, 157);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, bx, 161);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xf1f5f9), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0xe2e8f0), 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_add_event_cb(btn, relay_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)b_relays[i]);
        s_btn_relays[i] = btn;

        s_btn_icons[i] = lv_label_create(btn);
        lv_label_set_text(s_btn_icons[i], b_icons[i]);
        lv_obj_set_style_text_color(s_btn_icons[i], lv_color_hex(0x94a3b8), 0);
        lv_obj_set_style_text_font(s_btn_icons[i], &lv_font_montserrat_28, 0);
        lv_obj_align(s_btn_icons[i], LV_ALIGN_TOP_MID, 0, 22);

        s_btn_lbl_name[i] = lv_label_create(btn);
        lv_label_set_text(s_btn_lbl_name[i], b_names[i]);
        lv_obj_set_style_text_font(s_btn_lbl_name[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_btn_lbl_name[i], lv_color_hex(0x475569), 0);
        lv_obj_align(s_btn_lbl_name[i], LV_ALIGN_CENTER, 0, 4);

        // ON/OFF status badge at bottom
        s_btn_lbl_status[i] = lv_label_create(btn);
        lv_label_set_text(s_btn_lbl_status[i], "OFF");
        lv_obj_set_style_text_font(s_btn_lbl_status[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_btn_lbl_status[i], lv_color_hex(0x94a3b8), 0);
        lv_obj_align(s_btn_lbl_status[i], LV_ALIGN_BOTTOM_MID, 0, -8);
    }
}

// ============================================================
// Public API
// ============================================================

// LVGL timer shim — lv_timer_cb_t passes a timer pointer we don't need.
static void display_update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    display_update();
}

esp_err_t display_init(void)
{
    ESP_ERROR_CHECK(lcd_init());
    ESP_ERROR_CHECK(touch_init());

    // Initialise esp_lvgl_port
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    // Add display.
    // DMA-capable internal SRAM is required for the i80 8080 parallel bus GDMA.
    // PSRAM buffers cause cache coherency issues with the i80 controller and
    // produce garbled pixels, so .buff_dma must remain set.
    // Fragmentation of the internal DMA heap is handled by a heap guard in
    // sensor_task (see main.c) which skips SD writes if the largest free DMA
    // block drops below 4 KB, preventing the SPI master NULL-deref crash.
    // Single buffer, 10 lines.  Double-buffering 20 lines consumed 38 KB of
    // DMA-capable internal SRAM, leaving the SPI master driver unable to
    // allocate its 512-byte per-transaction priv RX buffer even at 27 seconds
    // uptime.  Single buffer at 10 lines costs only 9.6 KB and is imperceptible
    // at a 500 ms UI refresh rate.
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_io_handle,
        .panel_handle  = s_panel,
        .buffer_size   = LCD_H_RES * 10,
        .double_buffer = false,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .rotation      = { .swap_xy = true, .mirror_x = true, .mirror_y = true },
        .flags         = { .buff_dma = 1 },
    };
    s_lv_display = lvgl_port_add_disp(&disp_cfg);

    // Add touch
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp    = s_lv_display,
        .handle  = s_touch,
    };
    lvgl_port_add_touch(&touch_cfg);

    // Build the dashboard UI (must be called with LVGL lock held)
    lvgl_port_lock(0);
    build_ui();
    // Register a 500 ms periodic update timer — runs inside lv_timer_handler(),
    // so no separate task or lock contention needed.
    lv_timer_create(display_update_timer_cb, 500, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Display + LVGL ready");
    return ESP_OK;
}

void display_set_device_name(const char *name)
{
    if (!s_lbl_title || !name) return;
    lv_label_set_text(s_lbl_title, name);
}

void display_update(void)
{
    sensor_data_t sd;
    sensor_manager_get(&sd);

    relay_status_t rs[RELAY_COUNT];
    relay_get_all_status(rs);

    char buf[32];
    lvgl_port_lock(0);

    // Sensor values — write just the number; unit is a separate static label
    if (sd.temp_rh_valid) {
        snprintf(buf, sizeof(buf), "%.1f", sd.temperature_c);
        lv_label_set_text(s_lbl_temp, buf);
        snprintf(buf, sizeof(buf), "%.0f", sd.humidity_pct);
        lv_label_set_text(s_lbl_hum, buf);
    } else {
        lv_label_set_text(s_lbl_temp, "--");
        lv_label_set_text(s_lbl_hum,  "--");
    }

    if (sd.co2_valid) {
        snprintf(buf, sizeof(buf), "%u", sd.co2_ppm);
        lv_label_set_text(s_lbl_co2, buf);
        // Green < 800 ppm, amber 800-1200, red > 1200
        uint32_t co2_col = (sd.co2_ppm < 800)  ? 0x16a34a :
                           (sd.co2_ppm < 1200) ? 0xf59e0b : 0xdc2626;
        lv_obj_set_style_text_color(s_lbl_co2, lv_color_hex(co2_col), 0);
    } else {
        lv_label_set_text(s_lbl_co2, "--");
        lv_obj_set_style_text_color(s_lbl_co2, lv_color_hex(0x1a7a3c), 0);
    }

    // Water level indicator
    if (sd.level_low) {
        lv_label_set_text(s_lbl_level, "LOW");
        lv_obj_set_style_text_color(s_lbl_level, lv_color_hex(0xdc8500), 0);
    } else {
        lv_label_set_text(s_lbl_level, "OK");
        lv_obj_set_style_text_color(s_lbl_level, lv_color_hex(0x0077c2), 0);
    }

    // Update relay buttons — solid green when ON, white when OFF
    static const relay_id_t b_relays[] = {
        RELAY_HEATER, RELAY_HUMIDIFIER, RELAY_FAN, RELAY_LIGHT
    };
    for (int i = 0; i < 4; i++) {
        bool on = rs[b_relays[i]].state;
        lv_obj_set_style_bg_color(s_btn_relays[i],
            on ? lv_color_hex(0x16a34a) : lv_color_hex(0xffffff), 0);
        lv_obj_set_style_border_color(s_btn_relays[i],
            on ? lv_color_hex(0x15803d) : lv_color_hex(0xe2e8f0), 0);
        uint32_t icon_col = on ? 0xffffff : 0x94a3b8;
        uint32_t name_col = on ? 0xffffff : 0x475569;
        uint32_t stat_col = on ? 0xdcfce7 : 0x94a3b8;
        lv_obj_set_style_text_color(s_btn_icons[i],      lv_color_hex(icon_col), 0);
        lv_obj_set_style_text_color(s_btn_lbl_name[i],   lv_color_hex(name_col), 0);
        lv_obj_set_style_text_color(s_btn_lbl_status[i], lv_color_hex(stat_col), 0);
        lv_label_set_text(s_btn_lbl_status[i], on ? "ON" : "OFF");
    }

    // Clock — only show real time once NTP has synced (time > year 2020)
    time_t now_t = time(NULL);
    if (now_t > 1577836800 && s_lbl_time) {  // after 2020-01-01
        struct tm now_tm;
        localtime_r(&now_t, &now_tm);
        char tbuf[6];
        snprintf(tbuf, sizeof(tbuf), "%02d:%02d", now_tm.tm_hour, now_tm.tm_min);
        lv_label_set_text(s_lbl_time, tbuf);
    }

    bool healthy = sensor_manager_is_healthy();
    bool test_mode = controller_get_test_mode();
    bool door_open = controller_is_door_open();
    bool ap_mode   = (wifi_manager_get_mode() == WM_MODE_SOFTAP);
    if (test_mode) {
        lv_label_set_text(s_lbl_status, "TEST");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xf59e0b), 0);
    } else if (ap_mode) {
        lv_label_set_text(s_lbl_status, "AP MODE");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xf59e0b), 0);
    } else if (door_open) {
        lv_label_set_text(s_lbl_status, "DOOR OPEN");
        lv_obj_set_style_text_color(s_lbl_status, lv_color_hex(0xf59e0b), 0);
    } else {
        lv_label_set_text(s_lbl_status, healthy ? "OK" : "FAULT");
        lv_obj_set_style_text_color(s_lbl_status,
            lv_color_hex(healthy ? 0x16a34a : 0xdc2626), 0);
    }

    lvgl_port_unlock();
}
