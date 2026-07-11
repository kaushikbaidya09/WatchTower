/*!
    \file   main.c
    \brief  WallTick application entry point.

    Task layout
    -----------
    Core 0:  wt_task_wifi   (WiFi driver + NTP)
             wt_task_web    (HTTP management server)
    Core 1:  wt_task_led    (WS2812 render loop)
             wt_task_sound  (buzzer driver)
             app_main       (display logic — see below; never returns, so it
                             doubles as the display-request task instead of
                             idling once setup is done)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_web.h"
#include "wt_app_led.h"
#include "wt_app_settings.h"
#include "wt_seg_display.h"
#include "wt_app_sound.h"
#include "wt_app_time.h"

#define WT_MAIN_POLL_MS 1000

/* ------------------------------------------------------------------ */
/*  Main display task                                                    */
/* ------------------------------------------------------------------ */

/*!
    \brief  Field-by-field wt_segd_request_t comparison.

    A raw memcmp() over the struct is unsafe here: compiler-inserted padding
    between the mixed-size members (enum, int, char[], uint8_t[], bools) is
    implementation-defined and can cause spurious or missed change detection.
 */
static bool segd_request_equal(const wt_segd_request_t *a, const wt_segd_request_t *b)
{
    return (a->mode == b->mode) &&
           (a->value == b->value) &&
           (strncmp(a->text, b->text, sizeof(a->text)) == 0) &&
           (memcmp(a->raw, b->raw, sizeof(a->raw)) == 0) &&
           (a->demo_effect == b->demo_effect) &&
           (a->time_format == b->time_format) &&
           (a->colon == b->colon) &&
           (a->colon_blink == b->colon_blink) &&
           (a->anim == b->anim) &&
           (a->color_on.red == b->color_on.red) &&
           (a->color_on.green == b->color_on.green) &&
           (a->color_on.blue == b->color_on.blue) &&
           (a->color_off.red == b->color_off.red) &&
           (a->color_off.green == b->color_off.green) &&
           (a->color_off.blue == b->color_off.blue) &&
           (a->intensity == b->intensity);
}

/*!
    \brief  Display-request loop. Runs forever on the calling task; called
            from app_main() once setup is complete rather than as its own
            task, since app_main never returns and would otherwise just idle.
 */
static void wt_display_request_loop(void)
{
    wt_segd_request_t last_req = {0};
    bool has_last_req = false;

    wt_settings_register_notify_task(xTaskGetCurrentTaskHandle());

    while (1)
    {
        wt_settings_t cfg = wt_settings_get();

        wt_segd_request_t req = {
            .mode = WT_SEGD_MODE_TIME,
            .value = cfg.display_value,
            .time_format = cfg.time_format,
            .colon = true,
            .colon_blink = cfg.colon_blink,
            .anim = cfg.anim, /* already resolved by wt_settings normalize_settings() */
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

        /* Background full-strip animation overlay — independent of
           display_mode, so toggling it off just falls back to whichever
           mode (time/number/text) is already selected above. */
        if (cfg.bg_effect_en)
        {
            req.mode = WT_SEGD_MODE_DEMO;
            req.colon = false;
            req.colon_blink = false;
            req.demo_effect = wt_led_demo_effect_from_name(cfg.demo_effect);
        }

        bool changed = !has_last_req || !segd_request_equal(&last_req, &req);
        if (wt_segd_queue && (changed || req.mode == WT_SEGD_MODE_TIME))
        {
            xQueueOverwrite(wt_segd_queue, &req);
            last_req = req;
            has_last_req = true;
        }

        /* Wake immediately when settings change; WT_MAIN_POLL_MS is only a
           fallback so the TIME display keeps refreshing between changes. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WT_MAIN_POLL_MS));
    }
}

/* ------------------------------------------------------------------ */
/*  Application entry point                                           */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    setenv("TZ", "UTC0", 1);
    tzset();

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

    /* System time init */
    wt_time_init();

    /* Load persistent settings from NVS */
    wt_settings_init();

    /* Create shared queues/sync primitives before any producer/consumer
       task starts, so a fast producer never races a not-yet-created queue. */
    wt_segd_queue = xQueueCreate(1, sizeof(wt_segd_request_t));
    wt_buzzer_queue = xQueueCreate(5, sizeof(wt_sound_event_t));
    wt_segd_snapshot_init();
    if (!wt_segd_queue || !wt_buzzer_queue)
    {
        APPLOG_E("Failed to create shared queues — rebooting");
        esp_restart();
    }

    /* ---- Core 0 -------------------------------------------------- */
    BaseType_t task_ok = pdPASS;
    task_ok &= xTaskCreatePinnedToCore(wt_task_wifi, "WT_WIFI", 8192, NULL, 4, NULL, 0);
    task_ok &= xTaskCreatePinnedToCore(wt_task_web, "WT_WEB", 16384, NULL, 3, NULL, 0);

    /* ---- Core 1 -------------------------------------------------- */
    task_ok &= xTaskCreatePinnedToCore(wt_task_led, "WT_LED", 16384, NULL, 5, NULL, 1);
    task_ok &= xTaskCreatePinnedToCore(wt_task_sound, "WT_SOUND", 4096, NULL, 4, NULL, 1);

    if (task_ok != pdPASS)
    {
        APPLOG_E("Failed to create one or more application tasks — rebooting");
        esp_restart();
    }

    /* app_main's own task never returns, so it runs the display-request loop
       directly instead of spawning a 5th task purely to idle. Its stack size,
       core affinity, and priority are set via sdkconfig (CONFIG_ESP_MAIN_TASK_*)
       to match what the dedicated WT_MAIN task used: core 1, priority 4. */
    wt_display_request_loop();
}
