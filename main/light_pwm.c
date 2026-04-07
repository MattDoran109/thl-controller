// ============================================================
//  light_pwm.c — LED grow light PWM dimming control
//
//  LEDC timer 1, channels 1 & 2, 8-bit resolution.
//  Timer 0 / channel 0 are left free for the display backlight.
//
//  Actual panel protocol (verified by measurement):
//    Pin 2 = 12V always (LED supply rail, wired direct to 12V via relay)
//    Pin 1 = rows 1,3,5 GND return — sink LOW = ON, floating = OFF
//    Pin 3 = rows 2,4   GND return — sink LOW = ON, floating = OFF
//    Row selection is by Pin 1 / Pin 3 combination only.
//
//  Both channels are IDENTICAL circuits:
//    GPIO HIGH → ULN2003 sinks pin to GND → rows ON
//    GPIO LOW  → ULN2003 Hi-Z            → rows float → OFF
//    NON-inverted: 100% brightness = full duty = GPIO always HIGH
//
//  Channel 0  GPIO 11 → ULN2003 (3ch parallel) → Panel Pin 1 (rows 1,3,5)
//  Channel 1  GPIO 12 → ULN2003 (3ch parallel) → Panel Pin 3 (rows 2,4)
//
//  Colour temperature = row group selection:
//    Cool    (Full) = both channels → all 5 rows
//    Neutral (Med)  = channel 0 only → rows 1,3,5 + UV
//    Warm    (Low)  = channel 1 only → rows 2,4
// ============================================================

#include "light_pwm.h"
#include "config.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "light_pwm";

#define LEDC_SPEED_MODE     LEDC_LOW_SPEED_MODE
#define LEDC_TIMER_NUM      LEDC_TIMER_1
#define LEDC_BASE_CHANNEL   LEDC_CHANNEL_1
#define LEDC_RESOLUTION     LEDC_TIMER_8_BIT
#define LEDC_MAX_DUTY       256              // 2^8
#define LED_PWM_FREQ_HZ     1000            // 1 kHz — above flicker, MOSFET-friendly

// No frequency array needed — frequency is irrelevant to panel row selection.

static uint8_t s_brightness[LIGHT_PWM_CHANNELS] = {0};
static uint8_t s_colour_temp = LIGHT_COLOUR_COOL;

// ------------------------------------------------------------

esp_err_t light_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_SPEED_MODE,
        .timer_num       = LEDC_TIMER_NUM,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = LED_PWM_FREQ_HZ,
        .clk_cfg         = LEDC_USE_APB_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC timer config failed");

    // Channel 0 (Pin 1, rows 1,3,5): start at duty=0 → GPIO LOW → ULN2003 Hi-Z → rows OFF
    ledc_channel_config_t ch0 = {
        .speed_mode = LEDC_SPEED_MODE,
        .channel    = (ledc_channel_t)(LEDC_BASE_CHANNEL + 0),
        .timer_sel  = LEDC_TIMER_NUM,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PIN_LED_PWM,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch0), TAG, "LEDC ch0 config failed");

    // Channel 1 (Pin 3, rows 2,4): start at duty=0 → GPIO LOW → ULN2003 Hi-Z → rows OFF
    ledc_channel_config_t ch1 = {
        .speed_mode = LEDC_SPEED_MODE,
        .channel    = (ledc_channel_t)(LEDC_BASE_CHANNEL + 1),
        .timer_sel  = LEDC_TIMER_NUM,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PIN_LED_PWM_2,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch1), TAG, "LEDC ch1 config failed");

    ESP_LOGI(TAG, "LED PWM ready — GPIO%d (Pin1/rows135) GPIO%d (Pin3/rows24) at %d Hz",
             PIN_LED_PWM, PIN_LED_PWM_2, LED_PWM_FREQ_HZ);
    return ESP_OK;
}

// Both channels are identical ULN2003 low-side sinks — same polarity.
// High duty = GPIO HIGH = ULN2003 sinks pin = rows ON.
static void set_channel_duty(uint8_t channel, uint8_t pct)
{
    uint32_t duty = (uint32_t)pct * LEDC_MAX_DUTY / 100;
    ledc_set_duty(LEDC_SPEED_MODE, (ledc_channel_t)(LEDC_BASE_CHANNEL + channel), duty);
    ledc_update_duty(LEDC_SPEED_MODE, (ledc_channel_t)(LEDC_BASE_CHANNEL + channel));
}

void light_pwm_set_brightness(uint8_t channel, uint8_t pct)
{
    if (channel >= LIGHT_PWM_CHANNELS) return;
    if (pct > 100) pct = 100;
    s_brightness[channel] = pct;
    set_channel_duty(channel, pct);
}

// Route brightness to the correct channels based on colour temp mode:
//   Cool    = both channels (all 5 rows)
//   Neutral = channel 0 only (rows 1,3,5)
//   Warm    = channel 1 only (rows 2,4)
static void apply_brightness(uint8_t pct)
{
    s_brightness[0] = pct;
    switch (s_colour_temp) {
        case LIGHT_COLOUR_COOL:
            set_channel_duty(0, pct);
            set_channel_duty(1, pct);
            break;
        case LIGHT_COLOUR_NEUTRAL:
            set_channel_duty(0, pct);
            set_channel_duty(1, 0);   // rows 2,4 OFF
            break;
        case LIGHT_COLOUR_WARM:
            set_channel_duty(0, 0);   // rows 1,3,5 OFF
            set_channel_duty(1, pct);
            break;
        default:
            set_channel_duty(0, 0);
            set_channel_duty(1, 0);
            break;
    }
}

void light_pwm_set_all(uint8_t pct)
{
    apply_brightness(pct);
}

uint8_t light_pwm_get_brightness(uint8_t channel)
{
    if (channel >= LIGHT_PWM_CHANNELS) return 0;
    return s_brightness[channel];
}

void light_pwm_set_colour_temp(uint8_t mode)
{
    if (mode >= LIGHT_COLOUR_COUNT) return;
    if (mode == s_colour_temp) return;
    s_colour_temp = mode;
    ESP_LOGI(TAG, "Colour temp mode %d (%s)", mode,
             mode == LIGHT_COLOUR_COOL ? "Cool/all rows" :
             mode == LIGHT_COLOUR_NEUTRAL ? "Neutral/rows135" : "Warm/rows24");
    // Re-route brightness to correct channels for new mode.
    apply_brightness(s_brightness[0]);
}

uint8_t light_pwm_get_colour_temp(void)
{
    return s_colour_temp;
}
