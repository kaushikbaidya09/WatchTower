# WatchTower Firmware and Web Console - Software Design Description

| | |
|---|---|
| Document version | 1.1 |
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
- The web console (`web/index.html`, `app.js`, `style.css`), built into the firmware image.
- The simulation/test environments used during development (`simulation/wokwi`, `simulation/mock_device`).

Out of scope: PCB/schematic design, enclosure design, and manufacturing test procedures (not present in this repository).

### 1.3 Definitions, Acronyms, and Abbreviations

| Term | Meaning |
|---|---|
| RMT | ESP32 Remote Control peripheral, used here to bit-bang the WS2812 protocol |
| NVS | Non-Volatile Storage; ESP-IDF's flash key-value store, holds settings and WiFi profiles |
| OTA | Over-The-Air firmware update |
| STA / AP | WiFi Station mode / Access Point mode |
| DS3231 | I2C real-time clock IC used for timekeeping |
| Segment mask | A 7-bit value (bits A-G) selecting which segments of one 7-segment digit are lit |
| Anim effect | An animated colour pattern (rainbow, ripple, etc.) applied to the lit digit and colon segments |

### 1.4 References

- ESP-IDF documentation (RMT driver, esp_http_server, esp_wifi, NVS, temperature sensor).
- Source tree at `/home/kaushik/workspace/Templates/PWP` (this repository).
- `sdkconfig.s3-16mb` / `sdkconfig.s3-8mb` and `partitions/partitions-esp32-s3-16mb.csv` / `partitions/partitions-esp32-s3-8mb.csv` for the authoritative build/partition configuration of each board.

## 2. System Overview

WatchTower is a WiFi-connected desk clock built around a 4-digit, 7-segment display made of individually addressable WS2812 LEDs (7 segments x 2 LEDs/segment x 4 digits, plus a 2-LED colon = 58 LEDs total). An ESP32-S3 drives the display, keeps time from a DS3231 RTC (synced via NTP over WiFi), plays notification tones through a piezo buzzer, and hosts a single-page web console over its own WiFi AP/STA interfaces for configuration and live monitoring.

The device operates in combined AP+STA WiFi mode: it always exposes its own access point (for setup and fallback access) while also connecting to one of up to 8 stored STA profiles. The web console communicates with the firmware over a single WebSocket connection for all live state and commands; only page loads and firmware uploads use plain HTTP.

## 3. System Architecture

### 3.1 Hardware Overview

- MCU: ESP32-S3 (Xtensa, per `sdkconfig.defaults` `CONFIG_IDF_TARGET`).
- Display: 58x WS2812 addressable LEDs on GPIO 14, driven via the RMT peripheral (`components/wt_app_led`).
- RTC: DS3231 over I2C (`components/wt_app_time`), address `0x68`.
- Buzzer: piezo driven via LEDC PWM on GPIO 11 (`components/wt_app_sound`).
- On-chip temperature sensor: read directly by the web server component (`components/wt_app_web`) for the system-info panel.

### 3.2 Flash Partition Layout

From `partitions/partitions-esp32-s3-16mb.csv` (16 MB board; `partitions-esp32-s3-8mb.csv` is the same except `littlefs`):

| Partition | Type | Size | Purpose |
|---|---|---|---|
| `nvs` | data/nvs | 0x5000 | Settings + WiFi profiles (`wt_app_settings`, WiFi profile store) |
| `otadata` | data/ota | 0x2000 | OTA slot bookkeeping |
| `phy_init` | data/phy | 0x1000 | RF calibration data |
| `factory` | app | 2M | Factory firmware slot |
| `ota_0` / `ota_1` | app | 2M each | OTA update slots (dual-bank OTA) |
| `littlefs` | data/littlefs | 6M (1920K on the 8 MB board) | Unused. It held the web console until the console was built into the firmware; kept so existing devices keep their partition table. |

OTA updates go through `POST /api/ota` (Section 6.1) into the next OTA slot. The web console is part of the image, so one upload updates both, and a rollback restores the matching console too.

### 3.3 Task Layout

Defined in `main/main.c` and created in `app_main()`:

| Task | Core | Priority | Entry point | Component |
|---|---|---|---|---|
| WT_WIFI | 0 | 4 | `wt_task_wifi` | `wt_app_wifi` |
| WT_WEB | 0 | 3 | `wt_task_web` | `wt_app_web` |
| WT_LED | 1 | 5 | `wt_task_led` | `wt_app_led` |
| WT_SOUND | 1 | 4 | `wt_task_sound` | `wt_app_sound` |
| (main task) | 1 | 4 (`CONFIG_ESP_MAIN_TASK_*`) | `wt_display_request_loop` (called from `app_main`, never returns) | `main.c` |

`app_main()` never returns: after creating the four dedicated tasks it runs the display-request loop directly on the ESP-IDF main task rather than spawning a fifth task purely to idle, so the main task's own stack size/affinity/priority (set via `sdkconfig.defaults`) double as that loop's resource budget.

### 3.4 Component Dependency Graph

From each component's `CMakeLists.txt` `REQUIRES`/`PRIV_REQUIRES`:

```
main
 |-- wt_app_log      (no internal deps)
 |-- wt_app_time     -> wt_app_log
 |-- wt_app_sound    -> wt_app_log
 |-- wt_app_led      -> wt_app_log                    (+ esp_driver_rmt)
 |-- wt_app_settings -> wt_app_led, wt_app_log        (+ nvs_flash)
 |-- wt_app_wifi     -> wt_app_log, wt_app_settings, wt_app_time   (+ esp_wifi, nvs_flash, mdns)
 `-- wt_app_web      -> wt_app_log, wt_app_wifi, wt_app_settings, wt_app_led
                                                     (+ esp_http_server, app_update, cjson,
                                                        esp_driver_tsens)
```

`wt_app_log` is the only leaf dependency; every other component logs through it. `wt_app_web` sits at the top of the graph since it surfaces WiFi status, LED/display state, and settings to the network client.

## 4. Component Design

### 4.1 `wt_app_led` + `wt_seg_display` (WS2812 render task)

Files: `components/wt_app_led/wt_app_led.c`, `wt_seg_display.c`, and their headers.

`wt_seg_display` owns the display's data model: `wt_segd_request_t` (what to show), `wt_segd_frame_t` (the resolved per-digit segment masks), and `wt_segd_snapshot_t` (the last-rendered frame, colors included, exposed read-only via a mutex for the web server to report back to the UI). `wt_segd_prepare_frame()` converts a request's mode (NUMBER/TIME/TEXT/RAW) plus value/text/raw payload into segment masks using a 7-segment character lookup table (`wt_segd_char()`).

`wt_app_led` owns the render task (`wt_task_led`), which runs at a fixed 20 ms frame period (50 fps):

1. Pull the latest `wt_segd_request_t` off `wt_segd_queue` (non-blocking; a queue-of-1 with overwrite semantics, so only the newest request matters).
2. Slew `intensity` toward the target value (bounded step per frame) to avoid a hard brightness jump on setting changes.
3. Resolve the current frame via `wt_segd_prepare_frame()`, render each digit and the colon into the shared GRB pixel buffer (`render_digit`, `render_colon`), coloring each ON segment with `color_on` (scaled by intensity) and each OFF segment with `color_off`.
4. Publish a snapshot of what was just rendered (`wt_segd_snapshot_set`) for the web server to read.
5. Apply the effect. `anim_effect` mode 2 (`solid`) does nothing, leaving the digit/colon render from step 3. Any other mode (17 effects, listed in `wt_led_anim_effect_from_name()`) is drawn over the whole strip by `run_effects()`, then kept only on the LEDs lit in step 3 (`s_lit`). Each lit LED adds `color_on` in proportion to how dark the effect is there (`effect + color_on × (255 − peak) / 255`), so a lit segment never goes black, and the result is scaled by the brightness. Unlit segments return to `color_off`, and LEDs beyond the digit board are switched off, so the effect colours the displayed value instead of replacing it. `wt_led_anim_effect_from_name()` maps the web UI's effect name to this mode integer and defaults unknown/absent names to `solid`.
6. Transmit the pixel buffer over RMT (WS2812 bit timing), tolerating a transient RMT error by skipping the frame rather than rebooting.

This design (`solid` as the default no-op mode) replaced an earlier design where the overlay was a distinct `WT_SEGD_MODE_DEMO` display mode that replaced the digit render outright; see Section 9 for the rationale.

**Configurable LED count.** The 58-LED digit/colon layout itself is fixed (it's a physical wiring fact about the board: `s_digit_led_start`/`s_strip_pos_to_bit` in `wt_app_led.c`, and `WT_SEGD_TOTAL_LEDS` in `wt_seg_display.h`), but the *total* number of LEDs the render task drives is runtime-configurable via `wt_segd_request_t.led_count` / the `led_count` setting, clamped to `[WT_SEGD_TOTAL_LEDS, WT_SEGD_MAX_TOTAL_LEDS]` (58-100). This lets a builder wire up extra LEDs beyond the 58 needed for the digits (e.g. an ambient perimeter strip) and have them join in the spatial background effects. `s_pixels` and the effect layout tables (`coordsX`/`coordsY`/`angles`/`radii`) are all sized to the fixed ceiling `WT_SEGD_MAX_TOTAL_LEDS`; `build_led_layout()` copies the hand-calibrated 58-LED position data verbatim for the digit board and synthesizes an evenly-spaced ring layout for any extra LEDs, regenerating only when `led_count` actually changes. The RMT transmit length and every effect's loop bound use the currently-applied `led_count`, not the fixed digit count, so extra LEDs are actually driven. `wt_app_web.c` reports the valid range to the client as `min_leds`/`max_leds` in the one-time "info" WebSocket frame.

### 4.2 `wt_app_web` (HTTP + WebSocket server)

File: `components/wt_app_web/wt_app_web.c` (~1,880 lines, the largest component).

Responsibilities:

- Serves the SPA (`index.html`, `style.css`, `app.js`) over plain HTTP from the firmware image. `components/wt_app_web/CMakeLists.txt` gzips each file from `web/` (`tools/gzip_web.py`) and links it in (`target_add_binary_data`, symbols `_binary_<name>_gz_start`/`_end`); the server sends it with `Content-Encoding: gzip`. The ETag is the firmware's ELF hash, with `Cache-Control: no-cache`, so a browser that already has the files gets a 304 until the firmware changes.
- Accepts up to `WS_MAX_CLIENTS` (8) WebSocket clients and 12 open sockets (`CONFIG_LWIP_MAX_SOCKETS` is 16). A client that stops reading is dropped after the 3 s send timeout instead of stalling the broadcast to the others.
- Accepts firmware OTA uploads over HTTP (multipart, too large for a WebSocket frame) and writes them straight to the update partition.
- Answers captive-portal detection probes for iOS/macOS, Android/Chrome, Windows (NCSI and connect-test), and Firefox, each with the OS-specific expected response, plus a generic DNS-hijack responder so devices trigger the captive-portal flow when joining the AP.
- Hosts the single WebSocket endpoint (`/ws`) that carries all live telemetry and device commands (Section 6.2).
- Reads the on-chip temperature sensor directly (lazily installed on first use).
- Confirms a freshly updated firmware image once the server is up (`esp_ota_mark_app_valid_cancel_rollback()`), so an image that fails before that point is rolled back by the bootloader.
- Builds the periodic "full" state frame from WiFi status (`wt_app_wifi`), settings (`wt_app_settings`), display snapshot (`wt_seg_display`), and log entries (`wt_app_log`), only rebuilding the "wifi"/"power"/"settings" sub-objects when their respective generation counters change.

### 4.3 `wt_app_wifi` + `wt_app_settings` (WiFi driver and persistent settings)

Files: `components/wt_app_wifi/wt_app_wifi.c` and `components/wt_app_settings/wt_app_settings.c` (two components), with their headers.

`wt_app_wifi` runs combined AP+STA mode. The STA side rotates through up to `WT_WIFI_MAX_PROFILES` (8) stored credential sets, reconnecting automatically with a bounded per-profile retry count and rotating to the next profile once a profile's retries are exhausted or its disconnect reason indicates bad credentials/AP-not-found (`wifi_disconnect_reason_is_unrecoverable()`). Consecutive failed attempts are spaced by a backoff that doubles from 2 s to 30 s: each attempt makes the single radio scan every channel, which stalls the access point for its own clients, so a network that is out of range must not be retried in a tight loop. Profiles are persisted to NVS (`nvs_save_profiles`/`nvs_load_profiles`); every profile can be removed, including the last, in which case only the access point runs. `wt_wifi_add_profile()` rejects an SSID or passphrase of a length the radio could not use. A dedicated task (`ntp_sync_task`) performs blocking NTP sync (up to ~15 s) against the configured `ntp_server` setting first, with fixed fallbacks, without stalling the WiFi event handler; `wt_wifi_request_ntp_sync()` triggers it on demand.

`wt_wifi_scan_start()` runs a non-blocking scan for the console's "Available Networks" list. The single radio cannot scan while a connection attempt is in progress, so a requested scan abandons the attempt in flight and the reconnect task holds off until the results are in (or 12 s have passed); `WIFI_EVENT_SCAN_DONE` copies the de-duplicated results under the mutex and bumps `wt_wifi_get_scan_generation()`, which the web task watches.

The component also owns the device's network identity: the mDNS responder (`start_mdns()`, the device answers at `<hostname>.local`, default `watchtower`) and the access point's WPA2 password (default `watch@360`). Both are changed at runtime through `wt_wifi_set_hostname()` / `wt_wifi_set_ap_password()` and persisted in the WiFi NVS namespace. The access point password is never logged or sent to clients.

`wt_app_settings` is the single source of truth for all user-configurable behavior (display, clock, power) and persists to NVS as one struct (`wt_settings_t`, Section 5.2), guarded by an internal mutex. `wt_settings_set()` clamps/validates user-supplied fields (e.g. `display_mode`, `anim_effect`, `time_format`) via `normalize_settings()`, applies them in RAM at once, and bumps a generation counter (`wt_settings_get_generation()`) that `wt_app_web` polls to decide whether to re-send the settings sub-object. The write to NVS is deferred: `wt_settings_flush_if_due()`, called from the display-request loop, writes once the settings have been unchanged for 1.5 s, and `wt_settings_flush()` forces it before a reboot. Dragging a slider in the console therefore changes the display immediately without a flash write per step.

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

`app_main()` initializes logging, NVS, system time, and settings, creates the shared queues (`wt_segd_queue`, `wt_buzzer_queue`) before any task that could produce/consume from them, pins the four tasks to their cores, and then runs `wt_display_request_loop()` forever. That loop polls `wt_settings_get()`, builds a `wt_segd_request_t` from the current settings (mode, color, background-effect selection), and pushes it to `wt_segd_queue` whenever it changes (or every poll while in TIME mode, so the clock keeps advancing). It wakes immediately on a settings-change notification rather than relying solely on its 1 second poll fallback.

## 5. Data Design

### 5.1 Display data model (`wt_seg_display.h`)

- `wt_segd_request_t`: mode (`NUMBER`/`TIME`/`TEXT`/`RAW`) plus the corresponding payload (`value`, `text[5]`, `raw[4]`), colon state, on/off colors, master intensity, `anim_effect` (effect pattern index, applied to the lit segments in every mode), and `led_count` (total LEDs to drive, clamped to `[WT_SEGD_TOTAL_LEDS, WT_SEGD_MAX_TOTAL_LEDS]`).
- `wt_segd_frame_t`: resolved per-digit 7-bit segment masks plus colon on/off, produced by `wt_segd_prepare_frame()`.
- `wt_segd_snapshot_t`: the last frame actually rendered, including per-segment resolved colors and colon state, published for read-back by the web server.

### 5.2 Settings model (`wt_app_settings.h`)

`wt_settings_t` groups three areas, mirrored 1:1 by the web console's settings panel:

- Display: on/off colors, intensity, colon blink, scroll/transition flags, display mode/value/text, background-effect enable and pattern name, and LED count.
- Time/Clock: timezone, NTP server, time format (12/24), two alarms (time + enable), notification type/sound.
- Power: sleep mode, sleep timeout, battery alert threshold, display-dim and WiFi power-save flags.

### 5.3 WiFi model (`wt_app_wifi.h`)

- `wt_wifi_profile_t`: one SSID/password pair (up to `WT_WIFI_MAX_PROFILES` = 8 stored in NVS).
- `wt_wifi_status_t`: a point-in-time snapshot of STA (connected/IP/SSID/RSSI/active-profile-index) and AP (active/SSID/IP/client-count) state, returned by value from `wt_wifi_get_status()` so callers never touch shared state directly.

## 6. Interface Design

### 6.1 HTTP Endpoints

| Method | Path | Purpose |
|---|---|---|
| GET | `/` | Web console page (`index.html`, gzip, from the firmware image) |
| GET | `/style.css`, `/app.js` | Stylesheet and script, served like `/`. |
| POST | `/api/ota` | Upload a firmware image (which includes the web console). Rejected from the AP subnet (no other auth). |
| GET | various | Captive-portal probe responses (OS-specific; see Section 4.2) |
| GET | `/ws` | WebSocket upgrade (Section 6.2) |

### 6.2 WebSocket Protocol (`/ws`)

Server -> client frames:

- `{"type":"hello", "proto":1, "features":[...]}` - sent once immediately after connect, lets the client detect a firmware/UI protocol mismatch.
- `{"type":"info", ...}` - sent once per connection: chip model/cores/freq, flash size, reset reason, firmware version/build date/IDF version, OTA slot state. `app_version` is read at runtime from the running app's embedded `esp_app_desc_t` (via `esp_ota_get_app_description()`), which ESP-IDF populates at build time from `version.txt` at the project root (see `docs/GETTING_STARTED.md`).
- `{"type":"disp", "display": {...}}` - display-only frame, mirroring the render task's snapshot for the live preview. The snapshot is sampled every 50 ms but a frame is only sent when it differs from the last one sent, plus a keep-alive every 500 ms: a steady clock produces about two frames a second (the colon), an animated background effect up to 20.
- "full" frame, every 2 s, and immediately when a client connects or when settings or WiFi state change: system info (heap, RSSI, IP, temperature, CPU/flash usage) every time; `wifi`/`power`/`settings` sub-objects only rebuilt and sent when their underlying generation counter changed (or a client just connected), otherwise sent as JSON `null` meaning "unchanged"; incremental `logs.entries[]` since the last frame.

- `{"type":"scan", "scanning":bool, "networks":[{"ssid","rssi","channel","secure"}, ...]}` - sent when a scan starts (`scanning` true) and when its results are in, and to a client that connects later so the list survives a reload. Up to 16 networks, strongest first, one entry per SSID, hidden networks left out.

Client -> server commands (`{"cmd": "...", ...}`, no auth required beyond being on the network):

| Command | Effect |
|---|---|
| `ping` | Liveness check, returns `{"ack":"pong"}` |
| `settings` | Update one or more settings fields (see `wt_app_settings.h`) |
| `ntp_sync` | Optionally store a new `server`, then resync against the configured server |
| `reboot` | Acknowledge, then `esp_restart()` |
| `wifi {op:"add", ssid, password}` | Add a STA profile |
| `wifi {op:"del", index}` | Remove (forget) the profile at index; the last one may be removed too |
| `wifi {op:"connect", index}` | Connect to profile at index |
| `wifi {op:"scan"}` | Start a scan for nearby networks; the results follow in a `scan` frame |
| `wifi {op:"hostname", name}` | Rename the device on mDNS (`<name>.local`); 1-31 characters from a-z, 0-9 and `-` |
| `wifi {op:"ap_password", password}` | Change the access point's WPA2 password (8-63 characters) |

Every command is answered with `{"ack":"ok"}` or `{"ack":"err","msg":"..."}`. The console sends commands optimistically and shows the `msg` of an error reply as a toast. The `wifi` sub-object of the full frame carries `hostname` alongside the status and profile list.

## 7. Web Dashboard Design

Files: `web/index.html`, `app.js`, `style.css` (about 13 KB together once compressed, built into the firmware). Plain HTML, CSS and JavaScript with no framework, build step, web fonts or external requests, so the console also works on the access point without internet access.

Layout: five tabs (Display, Clock, Network, System, Logs) selected by the URL hash (`#network`). On wide screens the tabs sit in the header and the cards flow in a responsive grid; below 760 px the tabs become a bottom bar and the cards stack in one column. Light and dark themes follow the system setting, and the header button overrides it (stored in `localStorage`).

Key behaviour in `app.js`:

- `S` holds the client state: the last display snapshot, WiFi object, scan results, settings, log lines and connection timers.
- `connect()` opens `/ws`. It closes the socket when no frame arrives for 5 s and reconnects with a backoff of 1 s to 8 s. A page that stays hidden for 30 s closes its socket, freeing a client slot for other devices, and reconnects when shown again.
- `setSetting()` sends only the fields that changed, combined over a short delay (about 120 ms for sliders). Two people changing different settings therefore do not undo each other. A field edited in the last 1.5 s, or still focused, is not overwritten by the device's echo.
- `drawDisplay()` draws the live preview on a canvas, at device pixel ratio, from the "disp" frames. It uses the per-segment colours when the background effect sends them.
- Only settings the firmware acts on are shown. The stored but unused ones (scroll, transition, alarms, notifications, power) have no controls.

## 8. Simulation and Test Environments

- `simulation/wokwi/` (`wokwi.toml`, `diagram.json`): a Wokwi simulator configuration for running the firmware against a simulated ESP32-S3 and WS2812 strip without physical hardware.
- `simulation/mock_device/mock_server.py`: a standalone Python WebSocket/HTTP server that replays the same JSON schema as the real firmware (hello/info/disp/full frames), for developing and testing the web console without any device attached. It hand-duplicates the firmware's field names rather than sharing a schema definition; see Section 10 for the associated drift risk.

## 9. Key Design Decisions and Rationale

- **The effect is not a display mode.** Originally, enabling the background effect switched the display into a dedicated `WT_SEGD_MODE_DEMO` mode that replaced the digit/colon render outright. This was replaced with the current design: `run_effects()` always runs after the digit/colon render, and a no-op `solid` pattern (mode 2) is the default when no effect is selected. The effect is masked to the lit segments, so the value stays readable. This removes a mode-switching special case from both the render loop and the request-building logic in `main.c`, at the cost of one extra (cheap) comparison per frame when the effect is off.
- **Logging macro naming.** `APPLOG_I/E/W/D/V` were renamed to `wt_log_info/error/warn/debug/verbose` to match the rest of the codebase's `wt_`-prefixed naming convention; the underlying `APPLOG(type, format, ...)` implementation macro was left as an internal, unexported detail.
- **Documentation placement.** Function-level Doxygen doc comments (`/*! \brief ... */`) live only in `.c` files, directly above each function's definition, never in headers, so a header's declarations stay a pure interface listing. `static` (file-internal) functions carry a brief-only comment; non-static functions carry brief plus `\param[in]`/`\param[out]`/`\return` as applicable. Struct/type documentation remains in headers since the type itself is defined there.
- **No RTM/EARS requirements yet.** This project currently has no standalone requirements traceability matrix or EARS-format requirement set; behavior is captured here as as-built design instead. See Section 11.

## 10. Known Issues and Technical Debt

Open at the time of writing (verify still current before acting on any of these):

- No authentication: anyone on the STA network or the access point can use the console and every WebSocket command. The only restriction is that OTA uploads are refused from the access point subnet.
- After the STA interface gets an address, `softap_set_dns()` hands access point clients the upstream DNS server, but NAPT is not enabled, so those clients cannot reach it; captive-portal detection on the access point only works while the device has no STA connection.
- `anim_scroll` and `anim_transition` in `wt_settings_t`, the two alarms, the notification type/sound, and the power options are persisted but drive nothing yet; the web console no longer shows them.
- `app0_state`/`app1_state` and `cpu_freq_mhz` in the "info" frame, and `cpu_usage`/`flash_used_pct`/battery values in the "full" frame, are fixed placeholder values.
- A newly connected console only receives log lines produced after the previous full frame; earlier lines are not replayed (the device keeps one shared log cursor for all clients).
- The sources follow `docs/CODING_STANDARD.md` for file headers, comment style and formatting, but not yet its naming prefixes (`g_`, `p_`, `b_`) or its single-exit and constant-on-the-left rules.

Resolved since the previous baseline:

- Broadcast frames were written from the web task while the HTTP server task wrote command replies to the same socket. Interleaved frames made browsers drop the connection (`WS send failed ... ESP_FAIL`). Broadcasts now go through `httpd_queue_work()`, so every write happens on the server task; at most `WS_MAX_QUEUED` (4) wait at a time, and display frames are skipped while one is pending.
- The LED encoder ran from flash, so a settings save (flash write) could stall it and time out a frame. It now runs from IRAM with `CONFIG_RMT_TX_ISR_CACHE_SAFE`.
- A boolean setting missing from NVS (e.g. one added by a newer firmware) was loaded with a stale value from the previous key instead of keeping its default.
- The `ntp_server` setting had no effect, and the `ntp_sync` command stored the server without starting a sync.
- The access-point check on OTA uploads never matched: the server socket is IPv6-capable, so the peer address was read from the wrong bytes.
- An SSID containing a quote or backslash produced an unparseable "full" frame.
- `total_heap` was reported as free heap plus minimum free heap.
- The captive-portal DNS responder announced one answer for query types it did not answer.
- A stalled OTA client could hold the server task indefinitely; a failed web-asset upload left a truncated file.
- A request with a non-numeric or out-of-range settings value was cast straight to an integer type.
- The RTC read restored the time zone from a pointer that `setenv()` may already have overwritten.
- `led_count` was only clamped in the settings, not in the render task that sizes its loops and the RMT transmit with it.
- The access point password was written to the log, which every console client can read.
- Removed networks left their SSID and passphrase in NVS.
- Unused include and duplicate `WT_RTC_I2C_ADDR` in `wt_app_time.c`; `wt_segd_char()` and `wt_rtc_init()` had external linkage; the "display" sub-object was serialized by two separate implementations.

## 11. Open Items

- No EARS-format requirements set exists for this project; requirements are implicit in this design description and the code itself.
- No requirements traceability matrix (RTM) exists. If formal traceability is required going forward, an RTM should be created as a standalone spreadsheet mapping requirement IDs to the design sections and files above, and this document should be updated to reference it by path/baseline rather than duplicate its content.
- No unit tests exist. Static analysis is available as the `check` build target (cppcheck, currently clean) but is not gated by any CI.
