#pragma once

// ============================================================
//  config.h — THL Controller
//  WT32-SC01 PLUS (ESP32-S3)
// ============================================================
//
//  IMPORTANT: Verify all GPIO assignments against your wiring
//  before flashing.  The display & touch pins below are fixed
//  by the WT32-SC01 PLUS PCB and must not be used for anything
//  else.  The sensor / relay pins use the board's expansion
//  header — adjust if you have conflicts.
// ============================================================

#include <stdint.h>
#include <stdbool.h>

// ------------------------------------------------------------
// WiFi credentials & device identity
// (override at runtime via web UI; stored in NVS namespace "wifi")
// These compile-time values are only used on first boot or after
// a full factory reset — NVS values take precedence at runtime.
// ------------------------------------------------------------
#define WIFI_SSID_DEFAULT    ""               // set via web UI — stored in NVS
#define WIFI_PASS_DEFAULT    ""               // set via web UI — stored in NVS
#define WIFI_SSID2_DEFAULT   ""               // fallback SSID  (empty = none)
#define WIFI_PASS2_DEFAULT   ""               // fallback password
#define WIFI_HOSTNAME        "t-h-l-controller"
#define DEVICE_NAME_DEFAULT  "THL Controller"

// SoftAP fallback — used when both primary + fallback WiFi fail,
// or on first boot / after factory reset with no credentials stored.
#define WIFI_AP_SSID         "thlcontroller"
#define WIFI_AP_PASS         "setup1234"      // min 8 chars for WPA2; set "" for open
#define WIFI_AP_CHANNEL      1
#define WIFI_STA_MAX_RETRIES 5                // retries per network before trying next

// ------------------------------------------------------------
// WT32-SC01 PLUS — Display (ST7796UI, 8-bit Intel 8080 bus)
// These are FIXED by the PCB — do not change.
// Source: conf_WT32SCO1-Plus.h (sukesh-ak/ESP32-TUX, verified)
// NOTE: This is 8-BIT parallel, NOT 16-bit.
// NOTE: The TME datasheet PDF is for the original WT32-SC01
//       (ESP32 + SPI) and does NOT apply to the PLUS variant.
// ------------------------------------------------------------
#define LCD_H_RES           480
#define LCD_V_RES           320
#define LCD_BIT_WIDTH       8    // 8-bit MCU 8080 bus

#define PIN_LCD_BK_LIGHT    45   // Backlight PWM
#define PIN_LCD_CS          (-1) // CS not connected on PLUS PCB
#define PIN_LCD_DC          0    // RS / D/C line
#define PIN_LCD_WR          47   // Write clock
#define PIN_LCD_RST         4    // Reset

// 8-bit data bus D0-D7
#define PIN_LCD_D0          9
#define PIN_LCD_D1          46
#define PIN_LCD_D2          3
#define PIN_LCD_D3          8
#define PIN_LCD_D4          18
#define PIN_LCD_D5          17
#define PIN_LCD_D6          16
#define PIN_LCD_D7          15

// WT32-SC01 PLUS — Touch (FT5x06, I2C on I2C_NUM_0)
// FIXED by PCB — do not change.
#define PIN_TOUCH_SDA       6
#define PIN_TOUCH_SCL       5
#define PIN_TOUCH_RST       (-1)
#define PIN_TOUCH_INT       7
#define TOUCH_I2C_ADDR      0x38

// ------------------------------------------------------------
// Sensor I2C bus (I2C_NUM_1, separate from touch on I2C_NUM_0)
// GPIO 14 = SDA → expansion header pin 7
// GPIO 21 = SCL → expansion header pin 8
// MCP23017, SHT31, and SCD41 all share this bus.
// ------------------------------------------------------------
#define PIN_SENSOR_SDA      14   // Expansion header pin 7
#define PIN_SENSOR_SCL      21   // Expansion header pin 8
#define SENSOR_I2C_PORT     I2C_NUM_1
#define SENSOR_I2C_FREQ_HZ  100000

// Temp/RH sensor — DHT22 on single GPIO
// Change SENSOR_TEMPRH_TYPE to swap driver
// Options: SENSOR_TYPE_SHT31 | SENSOR_TYPE_BME280 | SENSOR_TYPE_DHT22
#define SENSOR_TYPE_SHT31   1
#define SENSOR_TYPE_BME280  2
#define SENSOR_TYPE_DHT22   3
#define SENSOR_TYPE_DHT11   4
#define SENSOR_TEMPRH_TYPE  SENSOR_TYPE_SHT31
// PIN_DHT22 not used (no DHT sensor fitted)
#define SHT31_I2C_ADDR      0x44
#define BME280_I2C_ADDR     0x76

// CO2 sensor — default SCD41
// Options: SENSOR_TYPE_SCD41 | SENSOR_TYPE_MHZ19
#define SENSOR_TYPE_SCD41   10
#define SENSOR_TYPE_MHZ19   11
#define SENSOR_CO2_TYPE     SENSOR_TYPE_SCD41
#define SCD41_I2C_ADDR      0x62

// MH-Z19 UART pins not used (no MH-Z19 fitted)

// Non-contact liquid level sensor (XKC-Y25)
// HIGH = liquid present at sensor height, LOW = level is below sensor.
// Place the sensor at the minimum fill line on the humidifier reservoir.
#define PIN_LEVEL_SENSOR    13   // Expansion header pin 6 (EXT_IO4)
#define LEVEL_SENSOR_LOW    1    // GPIO level that means "water is low"
                                     // 1 = ULN2003 inverts signal; GPIO LOW when water present,
                                     // GPIO HIGH (pull-up) when sensor disconnected → fail-safe LOW

// ------------------------------------------------------------
// MCP23017 I/O expander — relay and opto-coupler outputs
// I2C address 0x20 (A0/A1/A2 all tied LOW)
// Port A (GPA) → 4-relay board (active-LOW, JD-VCC from 5V)
// Port B (GPB) → 4-opto board
// ------------------------------------------------------------
#define MCP23017_I2C_ADDR        0x20

// Port A pin assignments (relay board)
#define MCP23017_GPA_HUMIDIFIER  0   // GPA0 — humidifier relay
#define MCP23017_GPA_PANEL_FAN   1   // GPA1 — cabinet cooling fan relay
#define MCP23017_GPA_FAN         2   // GPA2 — tent extraction fan relay
#define MCP23017_GPA_LIGHT       3   // GPA3 — 12V LED power relay

// Port B — GPB0 is an INPUT for the door micro-switch.
// Switch wired between GPB0 and GND; GPB0 has internal pull-up enabled.
// When door opens the switch activates and pulls GPB0 LOW → DOOR_OPEN_LEVEL 0.
// GPB1-7 available as spare outputs.
#define MCP23017_GPB_DOOR_SWITCH 0   // GPB0 — door micro-switch

// SSR heater: GPIO10 mirrors RELAY_HEATER for direct PWM-capable control
#define PIN_SSR_HEATER           10

// Relay/opto active level:
//   0 = active LOW  (MCP drives relay board directly — no ULN2003)
//   1 = active HIGH (ULN2003 in path: MCP HIGH -> ULN2003 OUT LOW -> relay fires)
#define RELAY_ACTIVE_LEVEL  1

// ------------------------------------------------------------
// LED grow light PWM output
// Channel 0 (GPIO 11): Panel Pin 1 — rows 1,3,5 via ULN2003 (single-invert)
// Channel 1 (GPIO 12): Panel Pin 2 — rows 2,4 P-MOSFET via double-inverter ULN2003
// ------------------------------------------------------------
#define PIN_LED_PWM    11
#define PIN_LED_PWM_2  12

// SD Card — WT32-SC01 PLUS onboard microSD socket (SPI mode, PCB-fixed pins)
// From schematic: SD_CS=GPIO41, SD_DI(MOSI)=GPIO40, SD_CLK=GPIO39, SD_DO(MISO)=GPIO38
#define PIN_SD_CLK    39   // SCLK
#define PIN_SD_MOSI   40   // SD_DI — data into card
#define PIN_SD_MISO   38   // SD_DO — data out of card
#define PIN_SD_CS     41   // Chip select

// Panel (cabinet) temperature sensor — LM75A on sensor I2C bus.
// Address pins A0/A1/A2 all tied to GND → address 0x48.
#define LM75A_I2C_ADDR  0x48
// GPIO 12 was previously used for DHT11 but is now LED PWM channel 2.
// #define PIN_PANEL_DHT  12    // disabled — GPIO 12 repurposed
#undef PIN_PANEL_DHT

// Panel temperature thresholds
#define DEFAULT_PANEL_TEMP_MAX_C    40.0f   // Turn panel fan ON above this
#define DEFAULT_PANEL_TEMP_HYST_C    5.0f   // Turn panel fan OFF below (max - hyst)

// Door micro-switch interlock.
// Switch installed on GPB0: LOW = door open (switch pulls GPB0 to GND when door opens).
// Set DOOR_SWITCH_INSTALLED 0 to compile out the interlock (e.g. switch not yet wired).
#define DOOR_SWITCH_INSTALLED  1
#define DOOR_OPEN_LEVEL        0    // GPIO level read when the door is open

// Sunrise/sunset ramp durations in minutes.  Set to 0 for instant on/off.
#define DEFAULT_LIGHT_SUNRISE_MIN  30
#define DEFAULT_LIGHT_SUNSET_MIN   30

// ------------------------------------------------------------
// Default environmental setpoints
// All stored in NVS and configurable via web UI
// ------------------------------------------------------------
#define DEFAULT_TEMP_SETPOINT_C     24.0f   // Target temperature °C
#define DEFAULT_TEMP_HYSTERESIS_C    1.0f   // ±1°C band
#define DEFAULT_TEMP_MAX_C          32.0f   // Safety cut-off (heater)

#define DEFAULT_HUM_SETPOINT_PCT    65.0f   // Target RH %
#define DEFAULT_HUM_HYSTERESIS_PCT   3.0f   // ±3% band
#define DEFAULT_HUM_MAX_PCT         85.0f   // Safety cut-off (humidifier)

#define DEFAULT_CO2_THRESHOLD_PPM   1200    // Fan turns on above this
#define DEFAULT_CO2_HYSTERESIS_PPM   100    // Fan turns off below threshold-hyst

#define DEFAULT_FAN_SCHED_ENABLED    true   // Timed ventilation active by default
#define DEFAULT_FAN_SCHED_ON_MIN     5      // Minutes fan runs per cycle
#define DEFAULT_FAN_SCHED_PERIOD_MIN 30     // Cycle period in minutes

#define DEFAULT_LIGHT_ON_HOUR        6      // Light schedule ON  (24h)
#define DEFAULT_LIGHT_OFF_HOUR      22      // Light schedule OFF (24h)

// Sensor read interval
#define SENSOR_READ_INTERVAL_MS    10000   // 10 seconds

// ------------------------------------------------------------
// NTP / Timezone
// POSIX timezone string — change to match your location:
//   UK (no DST):     "UTC0"
//   UK (with DST):   "GMT0BST,M3.5.0/1,M10.5.0"
//   US Eastern:      "EST5EDT,M3.2.0,M11.1.0"
//   US Pacific:      "PST8PDT,M3.2.0,M11.1.0"
//   Australia AEDT:  "AEST-10AEDT,M10.1.0,M4.1.0/3"
// ------------------------------------------------------------
#define NTP_SERVER   "pool.ntp.org"
#define TIMEZONE     "GMT0BST,M3.5.0/1,M10.5.0"   // UK with DST

// Watchdog — if no valid sensor reading in this time, safe-off all relays
#define SENSOR_WATCHDOG_MS         60000   // 60 seconds

// ------------------------------------------------------------
// Web server
// ------------------------------------------------------------
#define WEB_SERVER_PORT     80
