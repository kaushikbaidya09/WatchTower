# WatchTower

WiFi-connected desk clock firmware for ESP32-S3, driving a 4-digit WS2812 7-segment display (with configurable background animation effects) and a companion single-page web console for setup and live monitoring.

## Features

- 4-digit, 7-segment WS2812 LED display (time / number / text modes) with solid per-segment coloring.
- 17 colour effects on the displayed value (colour cycle, rainbow ring, rainbow flow, candy, plasma, galaxy, aurora, sunset, ripple, shockwave, comet, rain, fire, twinkle, pulse, heartbeat, police): the effect animates the lit segments only. The total LED count is configurable (58-100); LEDs beyond the digit board stay dark.
- DS3231 RTC with NTP sync (against the configured server), buzzer notifications, combined WiFi AP+STA mode with up to 8 stored networks.
- Found by name: the device answers at `http://watchtower.local` (mDNS), so its IP address does not need to be known. The name can be changed in the console's WiFi section.
- Single-page web console for desktop and phone (served from the device itself) over one WebSocket connection: live telemetry, settings, WiFi management (scan, add, connect, forget, access point password), and OTA firmware updates. The console is built into the firmware, so one `.bin` updates both, with rollback if a new firmware fails to boot.
- Wokwi simulator project and a standalone mock server for developing the web console without hardware.

## Quick Start

Requires ESP-IDF v6.0 and GNU Make. `tools/run.sh` finds ESP-IDF by itself;
the plain `idf.py` commands need its environment loaded in the terminal first.

```bash
tools/run.sh --build            # build the firmware
tools/run.sh --flash            # flash firmware and web console to the board
tools/run.sh --monitor          # serial monitor (exit with Ctrl+])
tools/run.sh --all              # clean, build from scratch, flash, monitor
tools/run.sh --flash --port /dev/ttyUSB0   # another serial port (default /dev/ttyACM0)

tools/format.sh                 # format C files and web/ (--check to only report)
cmake --build build --target check   # cppcheck static analysis, fails on any finding
cmake --build build --target docs    # Doxygen HTML → build/docs/html/index.html
```

The same without the script:

```bash
idf.py -G "Unix Makefiles" build     # first build: configure for GNU Make
idf.py build                         # later builds reuse that generator
idf.py -p <PORT> flash monitor
```

The build uses GNU Make, not Ninja: a build directory configured for Ninja is
refused with a message saying how to switch. Every build first runs
`tools/format.sh`: `.c`/`.h` files are formatted with clang-format and
the web console in `web/` with Prettier (`tools/config/.prettierrc`) before they
are compiled. Prettier is found on the PATH, in `node_modules`, or in the VS Code
Prettier extension; a missing tool is skipped with a warning.
The project's VS Code settings point the editor's format-on-save at the same
files in `tools/config/`, so saving a file formats it the way the build does.

Once flashed, join the WiFi network `WatchTower` (default password
`watch@360`, change it in the console's WiFi section) and open
`http://192.168.4.1`. After adding your own network there, the console is also
at `http://watchtower.local`.

See [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) for prerequisites, cloning, Wokwi simulation, the mock web server, and day-to-day device usage (WiFi setup, OTA updates).

## Documentation

- [`docs/GETTING_STARTED.md`](docs/GETTING_STARTED.md) - build, flash, simulate, and use the device.
- [`docs/DESIGN.md`](docs/DESIGN.md) - architecture, task layout, component design, data model, and the WebSocket/HTTP interface.
- [`docs/CODING_STANDARD.md`](docs/CODING_STANDARD.md) - file and function header formats, comment rules, naming, formatting.

## Project Layout

```
main/                   Application entry point (app_main, display-request loop)
components/
  wt_app_led/           WS2812 render task, 7-segment display model, effects
  wt_app_log/           Logging (console + in-memory ring buffer)
  wt_app_settings/      User settings, stored in NVS
  wt_app_sound/         Buzzer driver
  wt_app_time/          DS3231 RTC driver
  wt_app_web/           HTTP + WebSocket server, serves the web console
  wt_app_wifi/          WiFi (access point, saved networks, scan), mDNS, NTP
web/                    Web console (index.html, app.js, style.css), built into the firmware
tools/                  run.sh (build/flash/monitor), format.sh, gzip_web.py
  config/               clang-format, Prettier and Doxygen settings
simulation/
  mock_device/          Mock device server for working on the web console
  wokwi/                Wokwi simulator project
partitions/             Partition table (dual OTA; the 6 MB littlefs partition is unused)
docs/                   Design, getting started, coding standard
WatchTower.code-workspace  VS Code workspace: shared editor settings (open this file)
sdkconfig.defaults      The ESP-IDF options this firmware needs
version.txt             Firmware version (Semantic Versioning), read by the build
```

## Version

The firmware's version comes from [`version.txt`](version.txt) at the project root, in [Semantic Versioning](https://semver.org) format (`MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]`); the build rejects anything else and embeds it in the image, and the web console reports it as `app_version`. Bump this file when cutting a release.

## Known limitations

- **No login.** Anyone on the same network, or on the device's access point, can use the console and its commands. OTA uploads are refused from the access point.
- **Plain HTTP.** The console and OTA uploads are not encrypted.
- **Default access point password.** It is `watch@360` until changed in the console.
- **Alarms, notifications and the power options** are stored and shown in the console but do not drive anything yet.
- **No unit tests yet.**
