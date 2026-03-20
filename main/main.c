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
#include "driver/gpio.h"

#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_web.h"
#include "wt_app_led.h"
#include "wt_app_settings.h"
#include "wt_seg_display.h"

#define GPIO_OUTPUT_PIN 11
#define WT_MAIN_POLL_MS 1000

/* ------------------------------------------------------------------ */
/*  Main display task                                                    */
/* ------------------------------------------------------------------ */

static wt_segd_anim_t resolve_anim(const wt_settings_t *cfg)
{
    if (strcmp(cfg->reaction_effect, "rainbow") == 0)
    {
        return WT_SEGD_ANIM_RAINBOW;
    }
    if (cfg->anim_pulse)
    {
        return WT_SEGD_ANIM_PULSE;
    }
    return WT_SEGD_ANIM_SOLID;
}

void wt_task_main(void *pvParameters)
{
    wt_segd_request_t last_req = {0};
    bool has_last_req = false;

    while (1)
    {
        wt_settings_t cfg = wt_settings_get();

        wt_segd_request_t req = {
            .mode = WT_SEGD_MODE_TIME,
            .value = cfg.display_value,
            .time_format = cfg.time_format,
            .colon = true,
            .colon_blink = cfg.colon_blink,
            .anim = resolve_anim(&cfg),
            .color_on = cfg.color_on,
            .color_off = cfg.color_off,
            .intensity = cfg.intensity,
        };

        if (strcmp(cfg.display_mode, "number") == 0)
        {
            req.mode = WT_SEGD_MODE_NUMBER;
            req.colon = false;
            req.colon_blink = false;
        }
        else if (strcmp(cfg.display_mode, "text") == 0)
        {
            req.mode = WT_SEGD_MODE_TEXT;
            req.colon = false;
            req.colon_blink = false;
            strlcpy(req.text, cfg.display_text, sizeof(req.text));
        }
        else
        {
            req.mode = WT_SEGD_MODE_TIME;
        }

        bool changed = !has_last_req || memcmp(&last_req, &req, sizeof(req)) != 0;
        if (wt_segd_queue && (changed || req.mode == WT_SEGD_MODE_TIME))
        {
            xQueueOverwrite(wt_segd_queue, &req);
            last_req = req;
            has_last_req = true;
        }

        vTaskDelay(pdMS_TO_TICKS(WT_MAIN_POLL_MS));
    }
}

/* ------------------------------------------------------------------ */
/*  Application entry point                                           */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
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

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GPIO_OUTPUT_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    gpio_config(&io_conf);

    gpio_set_level(GPIO_OUTPUT_PIN, 1);
    printf("GPIO 11 HIGH\n");

    // Wait for 1 second
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Set GPIO low
    gpio_set_level(GPIO_OUTPUT_PIN, 0);
    printf("GPIO 11 LOW\n");

    /* ---- Core 0 -------------------------------------------------- */
    xTaskCreatePinnedToCore(wt_task_wifi, "WT_WIFI", 8192, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(wt_task_web, "WT_WEB", 16384, NULL, 3, NULL, 0);

    /* ---- Core 1 -------------------------------------------------- */
    xTaskCreatePinnedToCore(wt_task_led, "WT_LED", 16384, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(wt_task_main, "WT_MAIN", 4096, NULL, 4, NULL, 1);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
