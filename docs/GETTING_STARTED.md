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

- **ESP-IDF** (matching the target in `sdkconfig`: `esp32s3`). Install per Espressif's official ESP-IDF installation guide for your OS. Note the install path; it is needed for the build script below (referred to as `<esp-idf-path>`, e.g. `~/esp/esp-idf`).
- **Python 3** (used by ESP-IDF's build tooling, and by the mock web server in `simulation/mock_device/`; no extra Python packages are required for the mock server, it only uses the standard library).
- **A serial-capable USB connection** to the ESP32-S3 board for flashing.
- (Optional, for simulation without hardware) **Wokwi**: either the "Wokwi for VS Code" extension, or `wokwi-cli`, plus a Wokwi account/license as required by whichever tool you use.

## 3. Getting a Local Copy

If this project is tracked in a Git remote, clone it:

```bash
git clone <repository-url>
cd PWP
```

If you already have the project directory (e.g. copied locally, as in this workspace), just `cd` into it. The rest of this document assumes your shell is at the project root, i.e. the directory containing `CMakeLists.txt`, `main/`, `components/`, `spiffs_data/`, and `sdkconfig`.

## 4. Building the Firmware

### 4.1 Setting the Build Version

`version.txt` at the project root holds the firmware version string (e.g. `1.0.0`). ESP-IDF's build system reads this automatically and embeds it into the built app image as `PROJECT_VER`; the firmware reads it back at runtime via `esp_ota_get_app_description()->version` and reports it to the web console as `app_version` in the WebSocket "info" frame. Bump this file when cutting a new release; no other build-system changes are needed.

### 4.2 Using the provided build script (Linux)

`docs/tools/linux_build.sh` wraps the standard ESP-IDF build steps:

```bash
./docs/tools/linux_build.sh --esp-idf-path <esp-idf-path> --target esp32s3
```

- `--esp-idf-path <path>` (required): path to your ESP-IDF installation (the directory containing `export.sh`).
- `--target <target>` (optional): only needed the first time, or when switching chip targets; runs `idf.py set-target <target>` before building. This project's partition table (`partitions/partitions-esp32-s3.csv`) and `sdkconfig` are already configured for `esp32s3`.

The script sources `<esp-idf-path>/export.sh`, then runs `idf.py build` from the project root. Build output (including the `.elf` and `flasher_args.json` that the Wokwi config in Section 5 expects) lands in `build/`.

### 4.3 Using idf.py directly (any OS)

Equivalent manual steps, from the project root, after sourcing your ESP-IDF environment (`. <esp-idf-path>/export.sh` on Linux/macOS, or the ESP-IDF PowerShell/CMD environment on Windows):

```bash
idf.py set-target esp32s3   # first time only, or when switching targets
idf.py build
```

### 4.4 Flashing and Monitoring

With the board connected over USB:

```bash
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with the board's serial device (e.g. `/dev/ttyUSB0` or `/dev/ttyACM0` on Linux, `COMx` on Windows). `flash` writes the built factory-slot image; `monitor` attaches to the serial console (exit with `Ctrl+]`). The build also flashes the SPIFFS image containing the web console assets (`spiffs_data/`), via `spiffs_create_partition_image` in the top-level `CMakeLists.txt`.

Once flashed, subsequent firmware and web-asset updates can also be pushed over the air; see Section 6.

## 5. Simulating in Wokwi (no hardware required)

The `simulation/wokwi/` directory holds a ready-made Wokwi project:

- `diagram.json`: the simulated hardware (an ESP32-S3 DevKitC-1 board plus WS2812 LED strips wired to represent the display).
- `wokwi.toml`: points the simulator at `../../build/flasher_args.json` and `../../build/WatchTower.elf`, i.e. the output of Section 4 — **build the firmware first**, the simulator loads the same binary you would flash to real hardware.

To run it:

1. Build the firmware (Section 4) so `build/WatchTower.elf` and `build/flasher_args.json` exist.
2. Open the project in VS Code with the "Wokwi for VS Code" extension installed, and start a simulation from `simulation/wokwi/wokwi.toml` (or use `wokwi-cli` pointed at that same directory, if you use the CLI instead of the VS Code extension).

Note: the on-device web console still needs real WiFi connectivity to be reachable from a browser; if your simulation setup does not provide that, use the standalone mock server in Section 6 instead to work on the web console itself.

## 6. Running the Web Console Without a Device (mock server)

`simulation/mock_device/mock_server.py` is a standalone Python server that serves the real `spiffs_data/` files and replays the same WebSocket JSON schema as the firmware, so the web console can be developed/tested without any ESP32-S3 hardware at all.

```bash
python3 simulation/mock_device/mock_server.py [--host 127.0.0.1] [--port 8080]
```

Defaults: host `127.0.0.1`, port `8080`. Then open `http://<host>:<port>/` in a browser — this serves `index.html`/`app.js`/`style.css` straight from `spiffs_data/` and drives them with synthetic/sample data (see the script's own header comment for exactly which frames are sample vs. tick-driven). No extra Python packages are required.

Keep in mind the mock server hand-duplicates the firmware's JSON field names rather than sharing a schema (see `docs/DESIGN.md`, Section 10) — if you change a field name on the firmware side, update `mock_server.py` to match or the mock UI will silently drift from real-device behavior.

## 7. Using the Device Once Flashed

1. **First boot / no stored WiFi profile:** the device starts its own access point (AP mode is always active, per `docs/DESIGN.md` Section 4.3). Connect to it from a phone/laptop; joining the AP should trigger your OS's captive-portal prompt (the firmware answers the standard iOS/Android/Windows/Firefox probes for this), or navigate to the device's AP IP directly.
2. **Add a WiFi network:** in the web console's WiFi panel, add an SSID/password (stored to NVS, up to 8 profiles). The device will attempt to connect as a station while continuing to serve its own AP.
3. **Configure the clock:** the Display/Clock/Power panels cover color, brightness, animation, display mode (time/number/text), background overlay effect, timezone/NTP server, alarms, and power-saving options. Changes are sent live over the WebSocket and persisted to NVS.
4. **OTA updates:** from the Firmware panel, upload a new firmware `.bin` or a web asset (`index.html`/`style.css`/`app.js`) via `POST /api/ota?target=<name>`. Firmware updates land in the next OTA slot and the device reboots into it; web-asset updates overwrite the corresponding SPIFFS file in place. OTA uploads are rejected when the client is on the device's own AP subnet.

## 8. Where to Go Next

- Architecture, component responsibilities, and data/interface design: `docs/DESIGN.md`.
- Known issues and technical debt: `docs/DESIGN.md`, Section 10.
