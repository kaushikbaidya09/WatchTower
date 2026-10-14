# WatchTower - Getting Started

| | |
|---|---|
| Document version | 1.0 |
| Date | 2026-07-15 |
| Audience | Developers building, flashing, or simulating this firmware |

## 1. Introduction

### 1.1 Purpose

This document explains how to obtain, build, flash, and simulate the WatchTower firmware and web console, without needing to read the source tree first. For architecture and component design, see `docs/DESIGN.md`.

### 1.2 Scope

Covers: getting a local copy of the project, installing prerequisites, building and flashing the ESP32-S3 firmware, using the Wokwi hardware simulator, running the standalone web-console mock server, and day-to-day usage of the built system (WiFi setup, OTA updates).

## 2. Prerequisites

- **ESP-IDF v6.0.2** for `esp32s3`, installed with Espressif's installer (EIM) in its default location, `~/.espressif`. `tools/run.sh` and the VS Code workspace both look for it there.
- **Python 3** (used by ESP-IDF's build tooling, and by the mock web server in `simulation/mock_device/`; no extra Python packages are required for the mock server, it only uses the standard library).
- **A serial-capable USB connection** to the ESP32-S3 board for flashing.
- (Optional, for simulation without hardware) **Wokwi**: either the "Wokwi for VS Code" extension, or `wokwi-cli`, plus a Wokwi account/license as required by whichever tool you use.

## 3. Getting a Local Copy

If this project is tracked in a Git remote, clone it:

```bash
git clone <repository-url>
cd WatchTower
```

The rest of this document assumes your shell is at the project root, i.e. the directory containing `CMakeLists.txt`, `main/`, `components/`, `web/`, and `sdkconfig`.

### 3.1 Opening in VS Code

Open `WatchTower.code-workspace` (File → Open Workspace from File), not just the folder. It holds the shared settings, with no machine-specific paths:

- format on save with the build's rules: clang-format and Prettier settings from `tools/config/`;
- the ESP-IDF setup (`~/.espressif/v6.0.2/esp-idf`), target `esp32s3` and port `/dev/ttyACM0`;
- the debug configurations and the recommended extensions (ESP-IDF, clangd, C/C++, Prettier); VS Code offers to install them.

On first start the ESP-IDF extension writes `.vscode/settings.json` (the clangd path and build folder for this machine) and `.clangd`. Both are machine-specific and ignored by git. Change the port in your own `.vscode/settings.json` if your board is not `/dev/ttyACM0`.

## 4. Building the Firmware

### 4.1 Setting the Build Version

`version.txt` at the project root holds the firmware version string (e.g. `1.0.0`). ESP-IDF's build system reads this automatically and embeds it into the built app image as `PROJECT_VER`; the firmware reads it back at runtime via `esp_ota_get_app_description()->version` and reports it to the web console as `app_version` in the WebSocket "info" frame. Bump this file when cutting a new release; no other build-system changes are needed.

### 4.2 Using the provided script (Linux)

`tools/run.sh` wraps the standard ESP-IDF steps and finds the ESP-IDF
installation by itself (the installer's activation script in
`~/.espressif/tools`), so it works from a plain terminal:

```bash
tools/run.sh --build      # build the firmware
tools/run.sh --flash      # flash firmware and web console
tools/run.sh --monitor    # serial monitor (exit with Ctrl+])
tools/run.sh --all        # clean, build from scratch, flash, monitor
```

- The steps can be combined (`--build --flash --monitor`) and always run in the order clean, build, flash, monitor.
- `--clean` deletes `build/`, `managed_components/` (downloaded again on the next build, which needs a network connection) and `.cache/`. `sdkconfig` is kept.
- `--port <PORT>` selects the serial port (default `/dev/ttyACM0`).

Build output (including the `.elf` and `flasher_args.json` that the Wokwi config in Section 5 expects) lands in `build/`.

### 4.3 Using idf.py directly (any OS)

Equivalent manual steps, from the project root, after loading your ESP-IDF environment:

```bash
idf.py -G "Unix Makefiles" build   # first build: configure for GNU Make
idf.py build                       # later builds reuse that generator
```

The project is built with GNU Make. `idf.py` picks Ninja by default when it is installed, so the first configuration of a build directory needs `-G "Unix Makefiles"`; a directory configured for Ninja is refused with a message saying how to switch. `sdkconfig` and `sdkconfig.defaults` already select the `esp32s3` target and this project's partition table.

Every build first runs `tools/format.sh` over the sources. Static analysis and the API documentation are separate targets: `cmake --build build --target check` and `cmake --build build --target docs`.

### 4.4 Flashing and Monitoring

With the board connected over USB:

```bash
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with the board's serial device (e.g. `/dev/ttyUSB0` or `/dev/ttyACM0` on Linux, `COMx` on Windows). `flash` writes the built factory-slot image; `monitor` attaches to the serial console (exit with `Ctrl+]`). The web console (`web/`) is gzip-compressed and built into the firmware image, so there is no separate file system image to flash.

Once flashed, later updates can be installed over the air; see Section 6.

## 5. Simulating in Wokwi (no hardware required)

The `simulation/wokwi/` directory holds a ready-made Wokwi project:

- `diagram.json`: the simulated hardware (an ESP32-S3 DevKitC-1 board plus WS2812 LED strips wired to represent the display).
- `wokwi.toml`: points the simulator at `../../build/flasher_args.json` and `../../build/WatchTower.elf`, i.e. the output of Section 4 — **build the firmware first**, the simulator loads the same binary you would flash to real hardware.

To run it:

1. Build the firmware (Section 4) so `build/WatchTower.elf` and `build/flasher_args.json` exist.
2. Open the project in VS Code with the "Wokwi for VS Code" extension installed, and start a simulation from `simulation/wokwi/wokwi.toml` (or use `wokwi-cli` pointed at that same directory, if you use the CLI instead of the VS Code extension).

Note: the on-device web console still needs real WiFi connectivity to be reachable from a browser; if your simulation setup does not provide that, use the standalone mock server in Section 6 instead to work on the web console itself.

## 6. Running the Web Console Without a Device (mock server)

`simulation/mock_device/mock_server.py` is a standalone Python server that serves the real `web/` files and replays the same WebSocket JSON schema as the firmware, so the web console can be developed/tested without any ESP32-S3 hardware at all.

```bash
python3 simulation/mock_device/mock_server.py [--host 127.0.0.1] [--port 8080]
```

Defaults: host `127.0.0.1`, port `8080`. Then open `http://<host>:<port>/` in a browser — this serves `index.html`/`app.js`/`style.css` straight from `web/` and drives them with synthetic/sample data (see the script's own header comment for exactly which frames are sample vs. tick-driven). No extra Python packages are required.

Keep in mind the mock server hand-duplicates the firmware's JSON field names rather than sharing a schema (see `docs/DESIGN.md`, Section 10) — if you change a field name on the firmware side, update `mock_server.py` to match or the mock UI will silently drift from real-device behavior.

## 7. Using the Device Once Flashed

1. **First boot / no stored WiFi profile:** the device starts its own access point, `WatchTower` (AP mode is always active, per `docs/DESIGN.md` Section 4.3; the default password is `watch@360`). Connect to it from a phone/laptop; joining the AP should trigger your OS's captive-portal prompt (the firmware answers the standard iOS/Android/Windows/Firefox probes for this), or navigate to `http://192.168.4.1` directly.
2. **Add a WiFi network:** in the web console's WiFi section, press Scan to list nearby networks and pick one (an open network is added at once, a secured one asks for its password), or add an SSID/password by hand (stored to NVS, up to 8 profiles). The device will attempt to connect as a station while continuing to serve its own AP. "Remove" forgets a network, including the last one; with none stored the device is reached through its access point only.
3. **Find it on your network:** once joined, the console is at `http://watchtower.local` (mDNS), so the address your router assigned does not need to be known. The name can be changed in the WiFi section. If the name does not resolve (some networks block mDNS), use the IP address shown in the console or on the serial monitor.
4. **Change the access point password:** also in the WiFi section. Devices on the access point have to rejoin with the new one.
5. **Configure the clock:** the Display/Clock/Power panels cover color, brightness, animation, display mode (time/number/text), colour effect, timezone/NTP server, alarms, and power-saving options. Changes are sent live over the WebSocket and persisted to NVS.
6. **OTA updates:** in System → Update, install `build/WatchTower.bin` (`POST /api/ota`). It carries the web console too, so firmware and console update together. The image lands in the next OTA slot and the device reboots into it; if the new image resets before its web server comes up, the bootloader rolls back to the previous one (this needs a bootloader built with rollback, i.e. one USB flash of this version). OTA uploads are rejected when the client is on the device's own AP subnet.

## 8. Where to Go Next

- Architecture, component responsibilities, and data/interface design: `docs/DESIGN.md`.
- Known issues and technical debt: `docs/DESIGN.md`, Section 10.
