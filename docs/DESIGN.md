# WatchTower Firmware and Web Console - Software Design Description

| | |
|---|---|
| Document version | 1.0 |
| Date | 2026-07-15 |
| Status | Baseline (as-built) |
| Target | ESP32-S3 (WatchTower WS2812 7-segment clock) |

## 1. Introduction

### 1.1 Purpose

This document describes the as-built software design of the WatchTower firmware and its companion web console. It records the system architecture, task decomposition, component responsibilities, data structures, and communication interfaces so that engineers joining the project can orient themselves without reverse-engineering the source tree, and so that future changes can be evaluated against a known baseline.

This is a design document, not a requirements document. No EARS-format requirement set or requirements traceability matrix (RTM) currently exists for this project (see Section 11, Open Items). Where this document states behavior, it is describing what the code baseline at the time of writing does, not a specified requirement.

### 1.2 Scope

The scope covers:

- The ESP32-S3 firmware in `main/` and `components/` (WiFi, HTTP/WebSocket server, WS2812 LED render task, RTC/NTP time, buzzer, logging, persistent settings).
- The web console served from SPIFFS (`spiffs_data/index.html`, `app.js`, `style.css`).
- The simulation/test environments used during development (`simulation/wokwi`, `simulation/mock_device`).

Out of scope: PCB/schematic design, enclosure design, and manufacturing test procedures (not present in this repository).

### 1.3 Definitions, Acronyms, and Abbreviations

| Term | Meaning |
|---|---|
| RMT | ESP32 Remote Control peripheral, used here to bit-bang the WS2812 protocol |
| SPIFFS | SPI Flash File System; holds the web console's static assets |
| NVS | Non-Volatile Storage; ESP-IDF's flash key-value store, holds settings and WiFi profiles |
| OTA | Over-The-Air firmware update |
| STA / AP | WiFi Station mode / Access Point mode |
| DS3231 | I2C real-time clock IC used for timekeeping |
| Segment mask | A 7-bit value (bits A-G) selecting which segments of one 7-segment digit are lit |
| Anim effect / background overlay | A full-strip animation pattern (rainbow, ripple, etc.) rendered over the digit/colon LEDs |

### 1.4 References

- ESP-IDF documentation (RMT driver, esp_http_server, esp_wifi, NVS, temperature sensor).
- Source tree at `/home/kaushik/workspace/Templates/PWP` (this repository).
- `sdkconfig` and `partitions/partitions-esp32-s3.csv` for the authoritative build/partition configuration.

## 2. System Overview

WatchTower is a WiFi-connected desk clock built around a 4-digit, 7-segment display made of individually addressable WS2812 LEDs (7 segments x 2 LEDs/segment x 4 digits, plus a 2-LED colon = 58 LEDs total). An ESP32-S3 drives the display, keeps time from a DS3231 RTC (synced via NTP over WiFi), plays notification tones through a piezo buzzer, and hosts a single-page web console over its own WiFi AP/STA interfaces for configuration and live monitoring.

The device operates in combined AP+STA WiFi mode: it always exposes its own access point (for setup and fallback access) while also connecting to one of up to 8 stored STA profiles. The web console communicates with the firmware over a single WebSocket connection for all live state and commands; only static asset loading and OTA firmware/web-asset uploads use plain HTTP.

## 3. System Architecture

### 3.1 Hardware Overview

- MCU: ESP32-S3 (Xtensa, per `sdkconfig` `CONFIG_IDF_TARGET_ESP32S3`).
- Display: 58x WS2812 addressable LEDs on GPIO 14, driven via the RMT peripheral (`components/wt_app_led`).
- RTC: DS3231 over I2C (`components/wt_app_time`), address `0x68`.
- Buzzer: piezo driven via LEDC PWM on GPIO 11 (`components/wt_app_sound`).
- On-chip temperature sensor: read directly by the web server component (`components/wt_app_web`) for the system-info panel.

### 3.2 Flash Partition Layout

From `partitions/partitions-esp32-s3.csv`:

| Partition | Type | Size | Purpose |
|---|---|---|---|
| `nvs` | data/nvs | 0x5000 | Settings + WiFi profiles (`wt_app_settings`, WiFi profile store) |
| `otadata` | data/ota | 0x2000 | OTA slot bookkeeping |
| `phy_init` | data/phy | 0x1000 | RF calibration data |
| `factory` | app | 2M | Factory firmware slot |
| `ota_0` / `ota_1` | app | 2M each | OTA update slots (dual-bank OTA) |
| `spiffs` | data/spiffs | 6M | Web console static assets (`index.html`, `app.js`, `style.css`), flashed from `spiffs_data/` via `spiffs_create_partition_image` in the top-level `CMakeLists.txt` |

Firmware and web-asset OTA updates are both served through `POST /api/ota?target=<name>` (Section 6.1); firmware updates land in the next OTA slot, web-asset updates overwrite the SPIFFS files directly.

### 3.3 Task Layout

Defined in `main/main.c` and created in `app_main()`:

| Task | Core | Priority | Entry point | Component |
|---|---|---|---|---|
| WT_WIFI | 0 | 4 | `wt_task_wifi` | `wt_app_wifi` |
| WT_WEB | 0 | 3 | `wt_task_web` | `wt_app_web` |
| WT_LED | 1 | 5 | `wt_task_led` | `wt_app_led` |
| WT_SOUND | 1 | 4 | `wt_task_sound` | `wt_app_sound` |
| (main task) | 1 | 4 (`CONFIG_ESP_MAIN_TASK_*`) | `wt_display_request_loop` (called from `app_main`, never returns) | `main.c` |

`app_main()` never returns: after creating the four dedicated tasks it runs the display-request loop directly on the ESP-IDF main task rather than spawning a fifth task purely to idle, so the main task's own stack size/affinity/priority (set via `sdkconfig`) double as that loop's resource budget.

### 3.4 Component Dependency Graph

From each component's `CMakeLists.txt` `REQUIRES`/`PRIV_REQUIRES`:

```
main
 |-- wt_app_log      (no internal deps)
 |-- wt_app_time     -> wt_app_log
 |-- wt_app_sound    -> wt_app_log
 |-- wt_app_led      -> wt_app_log                    (+ esp_driver_rmt)
 |-- wt_app_wifi     -> wt_app_log, wt_app_led, wt_app_time   (+ esp_wifi, nvs_flash)
 |     `-- wt_app_settings (same component dir, shares wt_app_wifi's CMakeLists)
 `-- wt_app_web      -> wt_app_log, wt_app_wifi, wt_app_led   (+ spiffs, esp_http_server,
                                                                app_update, cjson, esp_driver_tsens)
```

`wt_app_log` is the only leaf dependency; every other component logs through it. `wt_app_web` sits at the top of the graph since it surfaces WiFi status, LED/display state, and settings to the network client.

## 4. Component Design

### 4.1 `wt_app_led` + `wt_seg_display` (WS2812 render task)

Files: `components/wt_app_led/wt_app_led.c`, `wt_seg_display.c`, and their headers.

`wt_seg_display` owns the display's data model: `wt_segd_request_t` (what to show), `wt_segd_frame_t` (the resolved per-digit segment masks), and `wt_segd_snapshot_t` (the last-rendered frame, colors included, exposed read-only via a mutex for the web server to report back to the UI). `wt_segd_prepare_frame()` converts a request's mode (NUMBER/TIME/TEXT/RAW) plus value/text/raw payload into segment masks using a 7-segment character lookup table (`wt_segd_char()`).

`wt_app_led` owns the render task (`wt_task_led`), which runs at a fixed 20 ms frame period (50 fps):

1. Pull the latest `wt_segd_request_t` off `wt_segd_queue` (non-blocking; a queue-of-1 with overwrite semantics, so only the newest request matters).
2. Slew `intensity` toward the target value (bounded step per frame) to avoid a hard brightness jump on setting changes.
3. Resolve the current frame via `wt_segd_prepare_frame()`, render each digit and the colon into the shared GRB pixel buffer (`render_digit`, `render_colon`), applying the selected animation (`WT_SEGD_ANIM_SOLID` / `PULSE` / `RAINBOW` / `WAVE` / `COLOR_FLOW`) per segment.
4. Publish a snapshot of what was just rendered (`wt_segd_snapshot_set`) for the web server to read.
5. Run the background overlay effect (`run_effects()`) unconditionally over the same pixel buffer. `anim_effect` mode 2 (`solid`) is a no-op, so when the overlay is disabled the digit/colon render from step 3 is left untouched; any other mode (rainbow ring, ripple, galaxy, XY flow, shockwave) overwrites the whole strip with that pattern. `wt_led_anim_effect_from_name()` maps the web UI's effect name to this mode integer and defaults unknown/absent names to `solid`.
6. Transmit the pixel buffer over RMT (WS2812 bit timing), tolerating a transient RMT error by skipping the frame rather than rebooting.

This design (overlay always runs, `solid` as the default no-op mode) replaced an earlier design where the overlay was a distinct `WT_SEGD_MODE_DEMO` display mode that replaced the digit render outright; see Section 9 for the rationale.

**Configurable LED count.** The 58-LED digit/colon layout itself is fixed (it's a physical wiring fact about the board: `s_digit_led_start`/`s_strip_pos_to_bit` in `wt_app_led.c`, and `WT_SEGD_TOTAL_LEDS` in `wt_seg_display.h`), but the *total* number of LEDs the render task drives is runtime-configurable via `wt_segd_request_t.led_count` / the `led_count` setting, clamped to `[WT_SEGD_TOTAL_LEDS, WT_SEGD_MAX_TOTAL_LEDS]` (58-100). This lets a builder wire up extra LEDs beyond the 58 needed for the digits (e.g. an ambient perimeter strip) and have them join in the spatial background effects. `s_pixels` and the effect layout tables (`coordsX`/`coordsY`/`angles`/`radii`) are all sized to the fixed ceiling `WT_SEGD_MAX_TOTAL_LEDS`; `build_led_layout()` copies the hand-calibrated 58-LED position data verbatim for the digit board and synthesizes an evenly-spaced ring layout for any extra LEDs, regenerating only when `led_count` actually changes. The RMT transmit length and every spatial effect's loop bound (`effect_rainbow_ring`/`ripple`/`galaxy`/`xy_flow`/`shockwave`) use the currently-applied `led_count`, not the fixed digit count, so extra LEDs are actually driven. `wt_app_web.c` reports the valid range to the client as `min_leds`/`max_leds` in the one-time "info" WebSocket frame.

### 4.2 `wt_app_web` (HTTP + WebSocket server)

File: `components/wt_app_web/wt_app_web.c` (~1,880 lines, the largest component).

Responsibilities:

- Serves the SPA (`index.html`, `style.css`, `app.js`) from SPIFFS over plain HTTP.
- Accepts firmware and web-asset OTA uploads over HTTP (binary multipart, cannot go over WebSocket).
- Answers captive-portal detection probes for iOS/macOS, Android/Chrome, Windows (NCSI and connect-test), and Firefox, each with the OS-specific expected response, plus a generic DNS-hijack responder so devices trigger the captive-portal flow when joining the AP.
- Hosts the single WebSocket endpoint (`/ws`) that carries all live telemetry and device commands (Section 6.2).
- Reads the on-chip temperature sensor directly (lazily installed on first use).
- Builds the periodic "full" state frame from WiFi status (`wt_app_wifi`), settings (`wt_app_settings`), display snapshot (`wt_seg_display`), and log entries (`wt_app_log`), only rebuilding the "wifi"/"power"/"settings" sub-objects when their respective generation counters change.

### 4.3 `wt_app_wifi` + `wt_app_settings` (WiFi driver and persistent settings)

Files: `components/wt_app_wifi/wt_app_wifi.c`, `wt_app_settings.c`, and their headers.

`wt_app_wifi` runs combined AP+STA mode. The STA side rotates through up to `WT_WIFI_MAX_PROFILES` (8) stored credential sets, reconnecting automatically with a bounded per-profile retry count and rotating to the next profile once a profile's retries are exhausted or its disconnect reason indicates bad credentials/AP-not-found (`wifi_disconnect_reason_is_unrecoverable()`). Profiles are persisted to NVS (`nvs_save_profiles`/`nvs_load_profiles`). A dedicated task (`ntp_sync_task`) performs blocking NTP sync (up to ~15 s) without stalling the WiFi event handler.

`wt_app_settings` is the single source of truth for all user-configurable behavior (display, clock, power) and persists to NVS as one struct (`wt_settings_t`, Section 5.2), guarded by an internal mutex. `wt_settings_set()` normalizes derived fields (e.g. `.anim`, which is computed from other settings rather than stored directly) via `normalize_settings()` before saving, and bumps a generation counter (`wt_settings_get_generation()`) that `wt_app_web` polls to decide whether to re-send the settings sub-object.

### 4.4 `wt_app_time` (RTC driver)

File: `components/wt_app_time/wt_app_time.c`.

Owns the DS3231 I2C driver (`wt_rtc_i2c_init`, BCD<->decimal conversion helpers) and exposes `wt_time_get_time()`/`wt_time_set_time()`. At boot, `wt_time_init()` reads the RTC and calls `settimeofday()` to seed the system clock (`wt_time_set_system()`), converting through a temporary `TZ=UTC0` switch since the RTC stores UTC. NTP sync itself is driven from `wt_app_wifi.c`, not this file; this component only owns the RTC hardware.

### 4.5 `wt_app_sound` (buzzer)

File: `components/wt_app_sound/wt_app_sound.c`.

A small consumer task (`wt_task_sound`) reads `wt_sound_event_t` values off `wt_buzzer_queue` and plays a fixed note sequence per event (`wt_sound[]`, a 2D table of frequency/duration pairs) through LEDC PWM (`buzzer_init`/`buzzer_play_freq`/`buzzer_stop`). `wt_sound_play_event()` is the only producer-side API; it is non-blocking (`xQueueSend` with zero timeout).

### 4.6 `wt_app_log` (logging)

Files: `components/wt_app_log/wt_app_log.c` and header.

Provides `wt_log_info/error/warn/debug/verbose()` function-like macros (formerly named `APPLOG_I/E/W/D/V`, renamed for consistency with the rest of the code's `wt_` naming convention). Each call formats a timestamped line, prints it to stdout, and appends it to a fixed-size ring buffer (`WT_LOG_BUF_ENTRIES` = 128 entries, `WT_LOG_ENTRY_LEN` = 200 bytes/entry) guarded by a mutex. `wt_log_read_json()` serializes only the entries newer than a caller-supplied sequence number, so `wt_app_web` can send incremental log updates in each periodic WebSocket frame instead of the whole buffer.

### 4.7 `main` (entry point and display-request loop)

File: `main/main.c`.

`app_main()` initializes logging, NVS, system time, and settings, creates the shared queues (`wt_segd_queue`, `wt_buzzer_queue`) before any task that could produce/consume from them, pins the four tasks to their cores, and then runs `wt_display_request_loop()` forever. That loop polls `wt_settings_get()`, builds a `wt_segd_request_t` from the current settings (mode, color, animation, background-effect selection), and pushes it to `wt_segd_queue` whenever it changes (or every poll while in TIME mode, so the clock keeps advancing). It wakes immediately on a settings-change notification rather than relying solely on its 1 second poll fallback.

## 5. Data Design

### 5.1 Display data model (`wt_seg_display.h`)

- `wt_segd_request_t`: mode (`NUMBER`/`TIME`/`TEXT`/`RAW`) plus the corresponding payload (`value`, `text[5]`, `raw[4]`), colon state, animation (`wt_segd_anim_t`), on/off colors, master intensity, `anim_effect` (background overlay pattern index, always applied regardless of mode), and `led_count` (total LEDs to drive, clamped to `[WT_SEGD_TOTAL_LEDS, WT_SEGD_MAX_TOTAL_LEDS]`).
- `wt_segd_frame_t`: resolved per-digit 7-bit segment masks plus colon on/off, produced by `wt_segd_prepare_frame()`.
- `wt_segd_snapshot_t`: the last frame actually rendered, including per-segment resolved colors and colon state, published for read-back by the web server.

### 5.2 Settings model (`wt_app_settings.h`)

`wt_settings_t` groups three areas, mirrored 1:1 by the web console's settings panel:

- Display: on/off colors, intensity, animation, colon blink, scroll/pulse/transition flags, reaction effect, display mode/value/text, background-effect enable and pattern name, and LED count.
- Time/Clock: timezone, NTP server, time format (12/24), two alarms (time + enable), notification type/sound.
- Power: sleep mode, sleep timeout, battery alert threshold, display-dim and WiFi power-save flags.

### 5.3 WiFi model (`wt_app_wifi.h`)

- `wt_wifi_profile_t`: one SSID/password pair (up to `WT_WIFI_MAX_PROFILES` = 8 stored in NVS).
- `wt_wifi_status_t`: a point-in-time snapshot of STA (connected/IP/SSID/RSSI/active-profile-index) and AP (active/SSID/IP/client-count) state, returned by value from `wt_wifi_get_status()` so callers never touch shared state directly.

## 6. Interface Design

### 6.1 HTTP Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Static SPA (`index.html` from SPIFFS) |
| GET | `/style.css`, `/app.js` | Static assets |
| POST | `/api/ota?target=<name>` | Upload firmware or a web asset; `<name>` is one of `firmware`, `index.html`, `style.css`, `app.js`. Rejected from the AP subnet for every target (no other auth). |
| GET | various | Captive-portal probe responses (OS-specific; see Section 4.2) |
| GET | `/ws` | WebSocket upgrade (Section 6.2) |

### 6.2 WebSocket Protocol (`/ws`)

Server -> client frames:

- `{"type":"hello", "proto":1, "features":[...]}` - sent once immediately after connect, lets the client detect a firmware/UI protocol mismatch.
- `{"type":"info", ...}` - sent once per connection: chip model/cores/freq, flash size, reset reason, firmware version/build date/IDF version, OTA slot state. `app_version` is read at runtime from the running app's embedded `esp_app_desc_t` (via `esp_ota_get_app_description()`), which ESP-IDF populates at build time from `version.txt` at the project root (see `docs/GETTING_STARTED.md`).
- `{"type":"disp", "display": {...}}` - display-only frame, ~20 fps, mirrors the render task's snapshot for a smooth live preview.
- Periodic "full" frame (~1 fps): system info (heap, RSSI, IP, temperature, CPU/flash/SPIFFS usage) every tick; `wifi`/`power`/`settings` sub-objects only rebuilt and sent when their underlying generation counter changed (or a client just connected), otherwise sent as JSON `null` meaning "unchanged"; incremental `logs.entries[]` since the last frame.

Client -> server commands (`{"cmd": "...", ...}`, no auth required beyond being on the network):

| Command | Effect |
|---|---|
| `ping` | Liveness check, returns `{"ack":"pong"}` |
| `settings` | Update one or more settings fields (see `wt_app_settings.h`) |
| `ntp_sync` | Trigger an NTP resync against the configured server |
| `reboot` | Acknowledge, then `esp_restart()` |
| `wifi {op:"add", ssid, password}` | Add a STA profile |
| `wifi {op:"del", index}` | Remove profile at index |
| `wifi {op:"connect", index}` | Connect to profile at index |

## 7. Web Dashboard Design

Files: `spiffs_data/index.html`, `app.js`, `style.css`.

The dashboard is a single-page, vanilla-JS application (no framework/build step) that renders directly against the WebSocket protocol above. Key structures in `app.js`:

- `S`: the single client-side state object (color, brightness, display mode/value/text, background-effect selection, LED count, connection state, WebSocket handle/backoff timer, log buffer). `S.minLeds`/`S.maxLeds` come from the one-time "info" frame's `min_leds`/`max_leds` and set the LED-count input's valid range client-side.
- `initWebSocket()`: connects to `/ws`, arms a watchdog timer that force-closes the socket if no frame arrives for `WS_WATCHDOG_MS` (5 s, since display frames should arrive every ~50 ms while connected), and reconnects with exponential backoff (`WS_RETRY_MIN_MS` 1 s up to `WS_RETRY_MAX_MS` 8 s).
- `handleWsMessage()`: dispatches on frame `type`/presence of `display`/`settings`/`wifi`/`power`/`logs` keys, updating only the DOM elements the current frame actually carries, and skipping a settings group's DOM sync entirely while the user has a pending debounced edit in flight for that group (so an in-progress edit is never clobbered by a stale server echo).
- A canvas-based 7-segment renderer (`renderDisplay`, `drawMask`/`drawMaskExact`) mirrors the LED task's rendering logic in 2D for the live preview and the settings-page preview.
- All settings changes are debounced (500-600 ms) before being sent, batched per settings group (display/clock/power).

## 8. Simulation and Test Environments

- `simulation/wokwi/` (`wokwi.toml`, `diagram.json`): a Wokwi simulator configuration for running the firmware against a simulated ESP32-S3 and WS2812 strip without physical hardware.
- `simulation/mock_device/mock_server.py`: a standalone Python WebSocket/HTTP server that replays the same JSON schema as the real firmware (hello/info/disp/full frames), for developing and testing the web console without any device attached. It hand-duplicates the firmware's field names rather than sharing a schema definition; see Section 10 for the associated drift risk.

## 9. Key Design Decisions and Rationale

- **Background overlay is unconditional, not a display mode.** Originally, enabling the background effect switched the display into a dedicated `WT_SEGD_MODE_DEMO` mode that replaced the digit/colon render outright. This was replaced with the current design: `run_effects()` always runs after the digit/colon render, and a no-op `solid` pattern (mode 2) is the default when no overlay is selected. This removes a mode-switching special case from both the render loop and the request-building logic in `main.c`, at the cost of one extra (cheap) function call per frame when the overlay is off.
- **Logging macro naming.** `APPLOG_I/E/W/D/V` were renamed to `wt_log_info/error/warn/debug/verbose` to match the rest of the codebase's `wt_`-prefixed naming convention; the underlying `APPLOG(type, format, ...)` implementation macro was left as an internal, unexported detail.
- **Documentation placement.** Function-level Doxygen doc comments (`/*! \brief ... */`) live only in `.c` files, directly above each function's definition, never in headers, so a header's declarations stay a pure interface listing. `static` (file-internal) functions carry a brief-only comment; non-static functions carry brief plus `\param[in]`/`\param[out]`/`\return` as applicable. Struct/type documentation remains in headers since the type itself is defined there.
- **No RTM/EARS requirements yet.** This project currently has no standalone requirements traceability matrix or EARS-format requirement set; behavior is captured here as as-built design instead. See Section 11.

## 10. Known Issues and Technical Debt

Recorded from a repository-wide dead-code/duplicate-code review at the time of writing (verify still current before acting on any of these):

- `components/wt_app_time/wt_app_time.c` includes `esp_sntp.h` without using it, and redefines `WT_RTC_I2C_ADDR`, which is already defined identically in its own header.
- `obtain_time()` in `wt_app_wifi.c` hardcodes its NTP server list and does not read the persisted `ntp_server` setting, so that setting currently has no effect despite being editable from the web UI.
- `anim_scroll` and `anim_transition` in `wt_settings_t` are persisted and exposed in the web UI but never read by `normalize_settings()` or elsewhere; currently inert.
- `wt_segd_char()` in `wt_seg_display.c` has external linkage but is only called within its own file; it should be `static`.
- `wt_app_web.c` maintains two independent implementations of the same "display" WebSocket sub-object schema (a cJSON-based builder and a manual string-building one); a field added to one is easy to miss in the other.
- `spiffs_data/index.html` duplicates its full navigation markup between the mobile dropdown and the sidebar (only differing by an added `closeMobileNav()` call).
- `spiffs_data/style.css` defines `.content-col`, which is not applied anywhere in the current markup.

## 11. Open Items

- No EARS-format requirements set exists for this project; requirements are implicit in this design description and the code itself.
- No requirements traceability matrix (RTM) exists. If formal traceability is required going forward, an RTM should be created as a standalone spreadsheet mapping requirement IDs to the design sections and files above, and this document should be updated to reference it by path/baseline rather than duplicate its content.
- Unit test coverage and static analysis gating are not present in this repository at the time of writing.
