# THL Controller

A full-featured temperature, humidity and CO₂ environment controller for the **WT32-SC01 Plus** (ESP32-S3 with 480×320 touchscreen). Originally built for mushroom cultivation but suitable for any grow environment requiring automated climate control.

![Board](https://img.shields.io/badge/board-WT32--SC01%20Plus-blue)
![IDF](https://img.shields.io/badge/ESP--IDF-v5.5.3-green)
![LVGL](https://img.shields.io/badge/LVGL-9.x-orange)

---

## Features

- **Touchscreen dashboard** — live temp / humidity / CO₂ readings, relay states, water level, clock
- **5 relay outputs** — heater, humidifier, circulation fan, panel fan, LED grow light
- **Setpoint control** — hysteresis-based control loops; fan cycle timer; light on/off schedule
- **Equipment failure alerts** — push notifications via [ntfy](https://ntfy.sh) when a device fails to reach its setpoint within a configurable time
- **SD card CSV logging** — one sample per minute; downloadable via the web UI
- **Web interface** — responsive single-page app; live readings, charts, settings, relay override, CSV download
- **Push notifications** — reboot alerts and equipment failure alerts via ntfy
- **Dual WiFi + SoftAP fallback** — primary + secondary SSID; falls back to AP mode for first-time setup; auto-reconnects indefinitely after transient dropouts
- **mDNS** — accessible at `http://<hostname>.local`
- **NTP time sync** with configurable timezone

---

## Hardware

| Component | Details |
|-----------|---------|
| Board | [WT32-SC01 Plus](https://www.wireless-tag.com/portfolio/wt32-sc01-plus/) |
| MCU | ESP32-S3, 2 MB PSRAM, 8 MB Flash |
| Display | ST7796 480×320 16-bit colour, Intel 8080 8-bit parallel |
| Touch | FT5x06 I²C capacitive |
| Sensors | SHT31 (temp/humidity), SCD41 (CO₂), DS18B20 (panel temp), ULN2003 float switch (water level) |
| Storage | MicroSD via SPI |
| Relays | 5× any 3.3 V-compatible relay module |

GPIO assignments are defined in [`main/config.h`](main/config.h) — verify against your wiring before flashing.

### Wiring

![Schematic](docs/wiring/THL%20Controller%20Schematic.png)

![Wiring Diagram](docs/wiring/THL%20Controller%20Wiring.png)

---

## Getting Started

### Prerequisites

- [ESP-IDF v5.5.x](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/)
- Python 3.11 (included with IDF installer)

### Build & Flash

```bash
idf.py build
idf.py -p PORT flash
idf.py -p PORT monitor
```

Or with the explicit esptool command (adjust `PORT`):

```bash
python -m esptool --chip esp32s3 -p PORT -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 8MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/al_wall_controller.bin
```

### First-time WiFi setup

On first boot (or with no credentials stored) the device starts a SoftAP:

| Setting | Value |
|---------|-------|
| SSID | `thlcontroller` |
| Password | `setup1234` |
| Config URL | `http://192.168.4.1` |

Connect to the AP, open the URL, enter your WiFi credentials and save. The device reboots and joins your network.

### Accessing the UI

Navigate to `http://<hostname>.local` (default hostname: `t-h-l-controller`) or the IP shown on the touchscreen.

---

## Project Structure

```
main/
  config.h          — GPIO pins, WiFi defaults, tunable constants
  main.c            — app_main, WiFi init, FreeRTOS tasks
  controller.c/h    — control loops, setpoints, relay scheduling
  sensor_manager.c/h— SHT31, SCD41, DS18B20, water level reads
  relay.c/h         — relay abstraction, manual override
  sd_logger.c/h     — SD mount, CSV logging, ring buffer
  web_server.c/h    — ESP-IDF HTTP server, all REST handlers + embedded HTML/JS
  display.c/h       — LVGL dashboard UI, touchscreen
  alert_manager.c/h — equipment failure detection, ntfy push notifications
  wifi_manager.c/h  — NVS credential storage
  notifier.c/h      — ntfy HTTP POST client
```

---

## Configuration

All runtime settings are stored in NVS and editable via the **Settings** page in the web UI:

- WiFi SSID (primary + fallback), hostname, device name, timezone
- Temperature, humidity, CO₂ setpoints and hysteresis bands
- Fan cycle on/off periods
- Light on/off schedule
- ntfy topic and server URL for push notifications
- Alert thresholds and failure-detection timeouts

---

## Release History

| Tag | Description |
|-----|-------------|
| `v1.5-wifi-reconnect` | WiFi reconnects indefinitely after transient dropouts |
| `v1.4-dma-fix` | Fix DMA heap exhaustion / SD card crash; reduce LVGL buffer |
| `v1.3.1-fail-alerts-fix` | Equipment fail timers persist through fan-cycle relay-off periods |
| `v1.3-fail-alerts` | Equipment failure alerts; water level display in minutes |
| `v1.2-alerts` | Configurable alert conditions |
| `v1.1-ntfy-working` | ntfy push notifications via PSRAM TLS buffers |
| `v1.0-stable` | Initial stable release |

---

## Dependencies

Managed via `idf_component.yml`:

- `espressif/esp_lcd_st7796`
- `espressif/esp_lcd_touch_ft5x06`
- `espressif/esp_lvgl_port`
- `lvgl/lvgl` ≥ 9.x
- `espressif/mdns`
