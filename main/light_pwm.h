#pragma once

// ============================================================
//  light_pwm.h — LED grow light PWM dimming control
//
//  Uses ESP32-S3 LEDC peripheral (timer 1, channels 1-4).
//  Frequency: 100 kHz (matches original controller hardware).
//
//  Signal path through ULN2003 inverting driver:
//    ESP32 GPIO HIGH  ->  ULN2003 OUT LOW  ->  panel signal  0V  -> cool/off
//    ESP32 GPIO LOW   ->  ULN2003 OUT HiZ  ->  panel pull-up ~7V -> warm/bright
//
//  Panel protocol: average voltage = colour temp + intensity together.
//    0V average = cool white, minimum brightness
//    ~7V average = warm white, maximum brightness
//
//  Therefore the duty cycle is INVERTED: 100% brightness = 0% GPIO duty.
//    set_brightness(ch, 100) -> duty=0   -> GPIO LOW  -> ULN2003 HiZ -> warm/max
//    set_brightness(ch, 0)   -> duty=MAX -> GPIO HIGH -> ULN2003 LOW -> cool/off
// ============================================================

#include <stdint.h>
#include "esp_err.h"

// Two PWM channels:
//   Channel 0 = GPIO PIN_LED_PWM   = Panel Pin 1 (rows 1,3,5 — ULN2003 single-invert)
//   Channel 1 = GPIO PIN_LED_PWM_2 = Panel Pin 2 (rows 2,4   — ULN2003 double-invert + P-MOSFET)
// Channel 1 is enabled only when rows 2&4 should be active (COOL or WARM mode).
#define LIGHT_PWM_CHANNELS  2

// Colour temperature modes (encoded as PWM carrier frequency).
//  0 = Cool / Full spectrum  — 100 kHz (rows 1–5 all on)
//  1 = Neutral               —  80 kHz (rows 1, 3, 5)
//  2 = Warm                  —  40 kHz (rows 2 & 4)
#define LIGHT_COLOUR_COOL     0
#define LIGHT_COLOUR_NEUTRAL  1
#define LIGHT_COLOUR_WARM     2
#define LIGHT_COLOUR_COUNT    3

// Initialise LEDC timer and channel.
// Channel starts at 0% (off).
esp_err_t light_pwm_init(void);

// Set brightness on one channel.  pct: 0 = off, 100 = full brightness.
void      light_pwm_set_brightness(uint8_t channel, uint8_t pct);

// Set all channels to the same brightness.
void      light_pwm_set_all(uint8_t pct);

// Query the last brightness set on a channel (0-100).
uint8_t   light_pwm_get_brightness(uint8_t channel);

// Change carrier frequency (colour temperature mode).
// 0=Cool/100kHz  1=Neutral/65kHz  2=Warm/40kHz
// Duty cycle (brightness) is preserved across the frequency change.
void      light_pwm_set_colour_temp(uint8_t mode);

// Query the current colour temperature mode (0-2).
uint8_t   light_pwm_get_colour_temp(void);
