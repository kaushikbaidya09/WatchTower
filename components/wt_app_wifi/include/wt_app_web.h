/*!
    \file   wt_app_web.h
    \brief  HTTP management web server running on the AP interface.

    Mounts SPIFFS (/spiffs) and serves a single-page application.
    REST endpoints allow full device management from any browser
    connected to the WallTick AP.

    Endpoints
    ---------
    GET  /                       Static SPA (index.html from SPIFFS)
    GET  /api/system             System info JSON
    GET  /api/logs?seq=N         Log entries since sequence N (JSON)
    GET  /api/wifi               WiFi status + profile list (JSON)
    POST /api/wifi/profile       Add a STA profile  {ssid, passwd}
    DELETE /api/wifi/profile?idx=N   Remove profile at index N
    POST /api/wifi/connect?idx=N Connect to profile N
    GET  /api/settings           Current settings JSON
    POST /api/settings           Update settings  (JSON body)
    POST /api/ota/firmware       Upload firmware binary → OTA + reboot
    POST /api/ota/webapp         Upload new index.html → write to SPIFFS
 */
#ifndef WT_APP_WEB_H
#define WT_APP_WEB_H

/** FreeRTOS task.  Pin to Core 0 alongside the WiFi task. */
void wt_task_web(void *pvParameters);

#endif /* WT_APP_WEB_H */
