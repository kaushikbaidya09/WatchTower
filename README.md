# WatchTower

WiFi-connected desk clock firmware for ESP32-S3, driving a 4-digit WS2812 7-segment display (with configurable background animation effects) and a companion single-page web console for setup and live monitoring.

## Features

- 4-digit, 7-segment WS2812 LED display (time / number / text modes), with per-segment color animation (solid, pulse, rainbow, wave, color flow).
- Always-on background overlay effects (rainbow ring, ripple, galaxy, XY flow, shockwave), with a runtime-configurable total LED count (58-100) so extra ambient LEDs beyond the digit board can join in.
- DS3231 RTC with NTP sync, buzzer notifications, combined WiFi AP+STA mode with up to 8 stored networks.
- Single-page web console (served from the device itself) over one WebSocket connection: live telemetry, settings, WiFi management, and OTA updates for both firmware and the web assets.
- Wokwi simulator project and a standalone mock server for developing the web console without hardware.

## Quick Start

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

See [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) for prerequisites, cloning, the provided build script, Wokwi simulation, the mock web server, and day-to-day device usage (WiFi setup, OTA updates).

## Documentation

- [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) - build, flash, simulate, and use the device.
- [`docs/DESIGN.md`](docs/DESIGN.md) - architecture, task layout, component design, data model, and the WebSocket/HTTP interface.

## Project Layout

```
main/                   Application entry point (app_main, display-request loop)
components/
  wt_app_led/           WS2812 render task + 7-segment display model
  wt_app_web/           HTTP + WebSocket server, web console host
  wt_app_wifi/          WiFi driver + persistent settings
  wt_app_time/          DS3231 RTC driver
  wt_app_sound/         Buzzer driver
  wt_app_log/           Logging (console + in-memory ring buffer)
spiffs_data/            Web console static assets (index.html, app.js, style.css)
simulation/
  wokwi/                Wokwi simulator project
  mock_device/          Standalone mock WebSocket server for UI development
partitions/             Custom ESP32-S3 partition table (dual OTA + SPIFFS)
docs/                   Design doc, getting-started guide, build tooling
version.txt             Firmware version string, read by ESP-IDF's build system
```

## Version

The firmware's version string comes from [`version.txt`](version.txt) at the project root, which ESP-IDF's build system embeds automatically; the web console reports it as `app_version`. Bump this file when cutting a release.
