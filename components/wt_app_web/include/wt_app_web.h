/*!
    \file   wt_app_web.h
    \brief  HTTP + WebSocket management web server running on the AP/STA
            interfaces.

    \details
    Mounts SPIFFS (/spiffs) and serves a single-page application.  Live
    telemetry and all state-changing device management goes over a single
    WebSocket connection; only static assets and OTA uploads use plain HTTP.

    HTTP endpoints
    --------------
    GET  /                        Static SPA (index.html from SPIFFS)
    GET  /style.css               Static CSS asset
    GET  /app.js                  Static JS asset
    POST /api/ota?target=<name>   Upload firmware/web asset (rejected from
                                   the AP subnet for every target; no other
                                   auth). <name> is one of: firmware,
                                   index.html, style.css, app.js.
    GET  /<captive-portal probes> OS captive-portal detection responses

    WebSocket  GET /ws
    -------------------
    Server → client:
      - One "hello" frame right after connect: {"type":"hello","proto":1,
        "features":[...]}. Lets the SPA detect a firmware/UI mismatch.
      - Periodic "disp" (display-only, ~20 fps) frames.
      - Periodic "full" frames (~1 fps): system info and logs every tick;
        the "wifi"/"power"/"settings" sub-objects are only rebuilt and
        sent when the underlying state actually changed (or a client just
        connected) otherwise they are JSON null, meaning "unchanged".

    Client → server commands (JSON {"cmd": "...", ...}) no auth required:
      ping                                Liveness check → {"ack":"pong"}
      settings                            Update settings (see wt_app_settings.h
                                           fields for accepted keys)
      ntp_sync                            Update NTP server + trigger a resync
      reboot                              Acknowledge then esp_restart()
      wifi {op:"add", ssid, password}     Add a STA profile
      wifi {op:"del", index}              Remove profile at index
      wifi {op:"connect", index}          Connect to profile at index
 */

#ifndef WT_APP_WEB_H
#define WT_APP_WEB_H

void wt_task_web(void *pvParameters);

#endif /* WT_APP_WEB_H */
