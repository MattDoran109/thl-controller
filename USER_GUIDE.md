# Al Wall Controller — User Guide

---

## Contents

1. [What It Does](#what-it-does)
2. [First-Time Setup](#first-time-setup)
3. [Connecting to the Interface](#connecting-to-the-interface)
4. [Dashboard](#dashboard)
5. [Device Controls](#device-controls)
   - [Heater](#heater)
   - [Humidifier](#humidifier)
   - [Fan / CO₂ Extraction](#fan--co-extraction)
   - [Light](#light)
6. [Settings — WiFi & Device](#settings--wifi--device)
7. [Settings — Push Notifications (ntfy)](#settings--push-notifications-ntfy)
8. [Settings — Alert Conditions](#settings--alert-conditions)
9. [Settings — Cabinet Fan](#settings--cabinet-fan)
10. [Data Logs](#data-logs)
11. [Hardware Test Page](#hardware-test-page)
12. [Factory Reset](#factory-reset)
13. [Troubleshooting](#troubleshooting)

---

## What It Does

The Al Wall Controller monitors and automatically controls the environment inside a growing cabinet or room. It measures temperature, humidity, and CO₂, and uses that data to drive four devices:

| Device | What it controls |
|--------|-----------------|
| **Heater** | Keeps temperature at your target |
| **Humidifier** | Keeps humidity at your target |
| **Fan** | Removes CO₂ / provides timed ventilation |
| **Light** | Runs a daily on/off schedule with optional sunrise/sunset dimming |

It logs all readings to an SD card, and can send instant alerts to your phone via the free **ntfy** app.

---

## First-Time Setup

### Step 1 — Connect to the controller's Wi-Fi

On first power-up (or after a factory reset), the controller creates its own Wi-Fi access point:

| Setting | Value |
|---------|-------|
| **Network name (SSID)** | `thlcontroller` |
| **Password** | `setup1234` |

Connect your phone or laptop to that network.

### Step 2 — Open the settings page

Open a browser and go to **http://192.168.4.1**, then click **Settings / Logs** (the gear / log icon at the top of the page).

### Step 3 — Enter your Wi-Fi credentials

Scroll down to the **WiFi & Device** card and fill in:

- **Primary SSID** — the name of your home/facility Wi-Fi network
- **Primary Password** — the password for that network
- **Fallback SSID / Password** *(optional)* — a second network to try if the first is unavailable (e.g. a mobile hotspot)

Click **Save WiFi & Device**. The controller will reboot and connect to your network.

### Step 4 — Find the controller's new address

Once connected to your normal Wi-Fi, the controller is accessible at:

- **http://\<hostname\>.local** — e.g. `http://t-h-l-controller.local` (works on most phones and laptops)
- Or by its IP address, which you can find in your router's device list

> **Tip:** Set a custom hostname in **WiFi & Device** settings so it is easy to remember, e.g. `growroom-1` → `http://growroom-1.local`

---

## Connecting to the Interface

Open a browser and navigate to the controller's address. The interface has two main pages:

| Page | How to get there |
|------|-----------------|
| **Dashboard** (live view) | Click the home icon / device name at the top |
| **Settings & Logs** | Click the log/gear icon at the top |

The interface updates the live readings automatically — no need to refresh.

---

## Dashboard

The dashboard shows the current state of the environment and all devices.

### Status bar (top right)

| Indicator | Meaning |
|-----------|---------|
| **HH:MM** (time) | Current device time (check timezone in Settings if wrong) |
| **SD: OK** (green) | SD card is mounted and logging |
| **SD: !** (orange) | SD card problem — hover/tap for details |
| **Door open** (yellow) | Door switch is reporting the door as open |
| **Sensors OK** (green) | All sensors are reading successfully |
| **⚠ Fault** (red) | One or more sensors have stopped responding |

### Environment readings (top row)

Displays the current **Temperature (°C)**, **Humidity (%)**, **CO₂ (ppm)**, and **Water Level**. The water level card turns orange when the level is low.

### Device cards

Four cards show the state of each controlled device:

- **ON / OFF** — whether the relay is currently switched
- **M** prefix (orange) — the device is in manual override mode (see [Hardware Test Page](#hardware-test-page))
- Tap/click any card to open its settings

If a **TEST MODE ACTIVE** banner appears at the top, all automatic scheduling is suspended and relays are under manual control — remember to exit test mode when finished.

---

## Device Controls

Tap any device card on the dashboard to open its settings modal. Changes take effect immediately and are saved to the device's memory.

### Heater

| Setting | Description |
|---------|-------------|
| **Temperature target** | The temperature the heater aims to maintain (°C) |
| **Hysteresis** | Dead-band on either side of the target. The heater turns ON when temperature drops to *target − hysteresis* and turns OFF when it rises to *target + hysteresis*. A value of 1.0°C means the heater cycles between 23 °C and 25 °C when the target is 24 °C. |

> **Safety:** If temperature exceeds the configured maximum (default 32 °C), the heater is locked off regardless of the setpoint.

---

### Humidifier

| Setting | Description |
|---------|-------------|
| **Humidity target** | The relative humidity (%) the humidifier maintains |
| **Hysteresis** | Dead-band around the target — works the same as the heater |

> **Interlocks — the humidifier is automatically disabled when:**
> - **Water level is low** — the water reservoir sensor has detected the tank is empty
> - **Fan is running** — to prevent humidity spiking during an extraction cycle

---

### Fan / CO₂ Extraction

The fan has two independent methods of control — both can be active at the same time.

#### CO₂-triggered

| Setting | Description |
|---------|-------------|
| **CO₂ threshold** | Fan turns ON when CO₂ reaches this level (ppm) |
| **CO₂ hysteresis** | Fan turns OFF when CO₂ drops this many ppm below the threshold |

*Example: threshold 1200 ppm, hysteresis 100 ppm → fan ON at 1200, OFF at 1100.*

#### Timed schedule

Useful for minimum fresh-air ventilation regardless of CO₂ level.

| Setting | Description |
|---------|-------------|
| **Schedule enabled** | Tick to activate timed ventilation |
| **Fan ON duration** | How long (minutes) the fan runs each cycle |
| **Cycle period** | Total time (minutes) between the start of each run |

*Example: ON duration 5 min, period 30 min → fan runs for 5 minutes every half-hour.*

---

### Light

| Setting | Description |
|---------|-------------|
| **ON time** | Time of day the light turns on (HH:MM, 24-hour) |
| **OFF time** | Time of day the light turns off (HH:MM, 24-hour) |
| **Sunrise ramp** | How long (minutes) the light takes to fade up to full brightness at turn-on. Set to 0 for instant-on. |
| **Sunset ramp** | How long (minutes) the light takes to fade down before turning off. Set to 0 for instant-off. |
| **Colour temperature** | **Full** — all LED rows active; **Neutral** — rows 1, 3, 5; **Warm** — rows 2, 4 |

The light will not run outside the ON–OFF window regardless of other settings.

---

## Settings — WiFi & Device

Accessible from the Settings/Logs page.

| Field | Description |
|-------|-------------|
| **Device Name** | Display name shown at the top of the dashboard |
| **Hostname** | Network hostname. Set this to something memorable — the device will then be reachable as `http://<hostname>.local` on your network |
| **Primary SSID / Password** | Your main Wi-Fi network |
| **Fallback SSID / Password** | Optional second network tried if the primary is unavailable |
| **Timezone** | Select your region so timestamps in logs and alerts are correct |

> Saving these settings **reboots the device**. Wait ~15 seconds, then reconnect.

**Wi-Fi reconnection behaviour:**
1. Device tries the primary network
2. If unavailable after several attempts, tries the fallback network
3. If both fail, returns to the built-in access point (`thlcontroller` / `setup1234`, IP `192.168.4.1`) so you can reconfigure without a factory reset

---

## Settings — Push Notifications (ntfy)

The controller can send instant alerts to your phone using the free [ntfy](https://ntfy.sh) service — no account or sign-up required.

### Setting up

1. **Install the ntfy app** on your phone:
   - Android: [Google Play](https://play.google.com/store/apps/details?id=io.heckel.ntfy)
   - iOS: [App Store](https://apps.apple.com/app/ntfy/id1625396347)

2. **Choose a topic name** — this is like a private channel name. Pick something unique that only you know, such as `growroom-doran-4821`. It is case-sensitive.

3. **Subscribe to the topic** in the ntfy app (tap the + button and enter your topic name).

4. **Enter the same topic name** in the controller's Push Notifications card and click **Save**.

5. Tap **Send Test Notification** to confirm it is working — you should receive a message on your phone within a few seconds.

> **Privacy note:** ntfy topics are not password-protected by default. Anyone who knows your topic name can send to it. Use a long, random-looking name to keep it private, or self-host the ntfy server.

---

## Settings — Alert Conditions

Configure when the controller sends push notifications. All settings are saved to the device's non-volatile memory and survive reboots.

> Alerts are only sent if a topic has been configured in Push Notifications.

---

### Temperature

Sends an alert when temperature goes outside the low/high band.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert type on or off |
| **Low threshold** | Alert fires if temperature drops below this value (°C) |
| **High threshold** | Alert fires if temperature rises above this value (°C) |
| **Repeat (min)** | How often to re-send the alert while the condition persists |

---

### Humidity

Same structure as temperature, but for relative humidity (%).

---

### CO₂

Same structure as temperature, but for CO₂ (ppm). Step size is 50 ppm.

---

### Water Level

Sends an alert when the water reservoir is empty.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert type on or off |
| **Repeat (min)** | How often to re-send the alert while the level remains low |

---

### Door Open

Sends an alert if the door is left open for too long.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert on or off |
| **Alert after (s)** | Seconds the door must be open before the first alert fires |
| **Repeat (s)** | How often to re-send while the door remains open |

---

### SD Card Failure

Sends an alert when the SD card cannot be mounted or becomes unreadable.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert on or off |
| **Repeat (min)** | How often to re-send while the card remains inaccessible |

---

### WiFi Failure

Sends an alert when the device loses its network connection.

> This alert can only be delivered once the device reconnects to the network. It is useful for detecting brief dropouts or for knowing when the device has recovered.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert on or off |
| **Repeat (min)** | How often to re-send while disconnected |

---

### Equipment Failure

Sends an alert when a device has been running for a sustained period but the environment has not improved — indicating a possible equipment fault.

#### Heater (can't reach temperature setpoint)

Fires if the **heater relay has been ON** but temperature is still below the setpoint after the configured time. The timer pauses during any fan cycle (when humidity and heating are interlocked) and only resets when the temperature actually reaches the setpoint.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert on or off |
| **Alert after (min)** | Minutes the heater must be running below setpoint before alerting |
| **Repeat (min)** | How often to re-send while the fault persists |

#### Humidifier (can't reach humidity setpoint)

Fires if the **humidifier relay has been ON** but humidity is still below the setpoint after the configured time. The timer holds during fan cycles (when the humidifier is interlocked off) and only resets when humidity reaches the setpoint.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert on or off |
| **Alert after (min)** | Minutes the humidifier must be running below setpoint before alerting |
| **Repeat (min)** | How often to re-send while the fault persists |

#### Fan / CO₂ not clearing

Fires if **CO₂ has remained above the threshold** for longer than the configured time — suggesting the extraction fan may have failed.

| Field | Description |
|-------|-------------|
| **Enabled** | Turn this alert on or off |
| **Alert after (min)** | Minutes CO₂ must stay above threshold before alerting |
| **Repeat (min)** | How often to re-send while CO₂ remains elevated |

---

### Power Fault / Reboot

Sends a single notification when the controller starts up (including after a power cut).

| Field | Description |
|-------|-------------|
| **Enabled** | Send a notification on every boot/restart |

---

## Settings — Cabinet Fan

The controller has a small internal fan to cool its own electronics. This is separate from the main extraction fan.

| Field | Description |
|-------|-------------|
| **Fan ON above (°C)** | Cabinet internal temperature at which the fan turns on |
| **Hysteresis (°C)** | The fan turns off when the temperature falls this many degrees below the threshold |

The current panel temperature is shown on this page in real time. Default: ON above 40 °C, OFF below 35 °C.

---

## Data Logs

The Logs page shows:

### Recent Readings table

Displays the last ~1 hour of sensor and relay data (one row every 10 seconds) stored in device memory. Columns:

| Column | Description |
|--------|-------------|
| Time | Timestamp (ISO format once synced to NTP, or `BOOT+Xs` before sync) |
| °C | Temperature |
| %RH | Relative humidity |
| CO₂ | CO₂ in ppm |
| Heat / Hum / Fan / Light | ● = relay ON, ○ = relay OFF |
| Panel°C | Cabinet internal temperature |

Click **Refresh** to load the latest data.

### Log files (SD card)

Lists all CSV files stored on the SD card, one per day, in the format `YYYY-MM-DD.csv`. Click any file link to download it. Files can be opened in Excel or any spreadsheet application.

> If the device had not yet synced to NTP when it started logging, data from around startup will be in a file called `boot_log.csv`.

**Remount SD** — if you removed the SD card briefly and reinserted it, use this button to make the controller re-read it without a full reboot.

---

## Hardware Test Page

Navigate to **http://\<device-address\>/test** (not linked from the main UI — intended for commissioning and fault-finding only).

On this page you can:

- **Enable Test Mode** — suspends all automatic control
- **Force any relay ON or OFF** individually
- **Adjust light brightness** with a slider and preset buttons
- **Change light colour temperature** (Full / Neutral / Warm)
- **View the cabinet temperature** in real time

> ⚠ Always **disable Test Mode** when you have finished testing. While Test Mode is active, the heater, fan, and humidifier will not respond to sensor readings.

---

## Factory Reset

Found at the bottom of the Settings/Logs page (red border card).

A factory reset **erases all stored settings**, including:
- Wi-Fi credentials
- All setpoints and schedules
- ntfy topic name
- All alert configuration

After the reset, the device reboots into access-point mode. Reconnect to `thlcontroller` (password `setup1234`) and visit `http://192.168.4.1` to reconfigure.

> Only use factory reset as a last resort. If you have only lost Wi-Fi access, the device will automatically fall back to its own access point — try that first.

---

## Troubleshooting

### Can't reach the interface

- Check that your device is on the same Wi-Fi network as the controller
- Try `http://t-h-l-controller.local` (or whatever hostname you set)
- If mDNS doesn't work (common on some Android/Windows setups), find the IP address from your router
- If the device has lost Wi-Fi, connect to the `thlcontroller` network (password `setup1234`) and visit `192.168.4.1`

### Time shown is wrong

Open Settings → WiFi & Device and set the correct **Timezone**. The time is synced from the internet (NTP) automatically once connected to Wi-Fi — it does not use a battery-backed clock.

### Logs show `BOOT+Xs` timestamps

The device had not yet synced its clock when those entries were recorded. This is normal at startup; once NTP sync completes the timestamps switch to normal date/time format automatically.

### SD card not logging

- Check the **SD: !** indicator on the dashboard for error details
- Ensure the card is fully seated
- Use the **Remount SD** button on the Logs page before reaching for the card
- The controller keeps the last ~1 hour of readings in memory regardless, so short card absences do not cause data loss
- FAT32 format is required (most cards ship this way; maximum tested size is 32 GB)

### No push notifications arriving

1. Confirm the topic name in Push Notifications settings matches exactly what you subscribed to in the ntfy app (case-sensitive)
2. Use **Send Test Notification** — if it returns an error code, check the device's Wi-Fi connection
3. Ensure the ntfy app has notification permission on your phone

### Heater / humidifier running but environment not improving

Check the **Equipment Failure** alerts are enabled in Alert Conditions — the controller will notify you if a device appears to be running without effect. Also verify the **water level** card — the humidifier is locked off when the tank is empty.

### Humidifier turns off during a fan cycle

This is by design. The fan interlock prevents the humidifier from running while extraction is active, to avoid false over-humidity readings and condensation. The humidifier resumes automatically when the fan stops.
