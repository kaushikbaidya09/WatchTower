/*!
    \file   main.c
    \brief  WallTick application entry point.

    Task layout
    -----------
    Core 0:  wt_task_wifi   (WiFi driver + NTP)
             wt_task_web    (HTTP management server)
    Core 1:  wt_task_main   (display logic)
             wt_task_led    (WS2812 render loop)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_web.h"
#include "wt_app_led.h"
#include "wt_app_settings.h"
#include "wt_seg_display.h"

/* ------------------------------------------------------------------ */
/*  Main display task                                                    */
/* ------------------------------------------------------------------ */

void wt_task_main(void *pvParameters)
{
    // APPLOG_I("---------- APP MAIN TASK STARTED ----------");

    while (1)
    {
        wt_settings_t cfg = wt_settings_get();

        wt_segd_request_t req = {
            .mode = WT_SEGD_MODE_TIME,
            .value = 0,
            .colon = true,
            .colon_blink = cfg.colon_blink,
            .anim = cfg.anim,
            .color_on = cfg.color_on,
            .color_off = cfg.color_off,
            .intensity = cfg.intensity,
        };

        if (wt_segd_queue)
        {
            xQueueOverwrite(wt_segd_queue, &req);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ------------------------------------------------------------------ */
/*  Application entry point                                              */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    /* Silence ESP-IDF component logs — our APPLOG handles output */
    esp_log_level_set("*", ESP_LOG_NONE);

    /* Initialise log ring buffer first (macros safe from here on) */
    wt_log_init();

    APPLOG_I("========== WATCHTOWER APPLICATION STARTED ==========");

    /* NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Load persistent settings from NVS */
    wt_settings_init();

    /* ---- Core 0 -------------------------------------------------- */
    xTaskCreatePinnedToCore(wt_task_wifi, "WT_WIFI", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(wt_task_web, "WT_WEB", 8192, NULL, 3, NULL, 0);

    /* ---- Core 1 -------------------------------------------------- */
    xTaskCreatePinnedToCore(wt_task_led, "WT_LED", 16384, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(wt_task_main, "WT_MAIN", 4096, NULL, 4, NULL, 1);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
