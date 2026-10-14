//=============================================================================
/*!
    \file    wt_app_web.h
    \brief   Web server: web console, uploads and WebSocket.
*/
//=============================================================================

// Serves the web console, which is built into the firmware image. Live
// data and every command go over one WebSocket connection; only the page
// files and firmware uploads use plain HTTP.
//
// HTTP endpoints
// --------------
// GET  /, /style.css, /app.js   Web console (gzip-compressed)
// POST /api/ota                 Firmware upload; also updates the console.
//                               Refused from the access point subnet; no
//                               other authentication.
// GET  /<captive-portal probes> OS captive-portal detection responses
//
// WebSocket  GET /ws
// -------------------
// Server → client:
//   - One "hello" frame right after connect: {"type":"hello","proto":1,
//     "features":[...]}. Lets the SPA detect a firmware/UI mismatch.
//   - "disp" (display-only) frames, sent when the display changes, at
//     most 20 a second, with a keep-alive every 500 ms.
//   - "full" frames every 2 s, and at once on connect or when settings /
//     WiFi state change: system info and logs every time;
//     the "wifi"/"power"/"settings" sub-objects are only rebuilt and
//     sent when the underlying state actually changed (or a client just
//     connected) otherwise they are JSON null, meaning "unchanged".
//
// Client → server commands (JSON {"cmd": "...", ...}) no auth required:
//   ping                                Liveness check → {"ack":"pong"}
//   settings                            Update settings (see wt_app_settings.h
//                                        fields for accepted keys)
//   ntp_sync                            Update NTP server + trigger a resync
//   reboot                              Acknowledge then esp_restart()
//   wifi {op:"add", ssid, password}     Add a STA profile
//   wifi {op:"del", index}              Remove profile at index
//   wifi {op:"connect", index}          Connect to profile at index
//   wifi {op:"scan"}                    Scan for nearby networks; results
//                                        follow in a {"type":"scan"} frame
//   wifi {op:"hostname", name}          Rename the device on mDNS
//   wifi {op:"ap_password", password}   Change the access point password
//
// Every command is answered with {"ack":"ok"} or {"ack":"err","msg":"..."}.

#ifndef WT_APP_WEB_H
#define WT_APP_WEB_H

void wt_task_web(void *pvParameters);

#endif /* WT_APP_WEB_H */
