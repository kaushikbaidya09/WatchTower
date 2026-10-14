//=============================================================================
/*!
    \file    main.c
    \brief   WatchTower entry point.
*/
//=============================================================================

// Tasks:
//   Core 0: wt_task_wifi (WiFi, NTP), wt_task_web (web server)
//   Core 1: wt_task_led (LED render loop), wt_task_sound (buzzer),
//           app_main (display-request loop; never returns)

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

/*!
    \brief  Compare two display requests field by field. memcmp() is not
            used because the struct contains padding bytes.
*/
static bool segd_request_equal(const wt_segd_request_t *a, const wt_segd_request_t *b)
{
    return (a->mode == b->mode) && (a->value == b->value) &&
           (strncmp(a->text, b->text, sizeof(a->text)) == 0) &&
           (memcmp(a->raw, b->raw, sizeof(a->raw)) == 0) && (a->anim_effect == b->anim_effect) &&
           (a->led_count == b->led_count) && (a->time_format == b->time_format) &&
           (a->colon == b->colon) && (a->colon_blink == b->colon_blink) &&
           (a->color_on.red == b->color_on.red) && (a->color_on.green == b->color_on.green) &&
           (a->color_on.blue == b->color_on.blue) && (a->color_off.red == b->color_off.red) &&
           (a->color_off.green == b->color_off.green) && (a->color_off.blue == b->color_off.blue) &&
           (a->intensity == b->intensity);
}

/*!
    \brief  Build the display request from the current settings and send it
            to the LED task whenever it changes. Never returns.
*/
static void wt_display_request_loop(void)
{
    wt_segd_request_t last_req = {0};
    bool has_last_req          = false;

    wt_settings_register_notify_task(xTaskGetCurrentTaskHandle());

    while (1)
    {
        wt_settings_t cfg = wt_settings_get();

        wt_segd_request_t req = {
            .mode        = WT_SEGD_MODE_TIME,
            .value       = cfg.display_value,
            .time_format = cfg.time_format,
            .colon       = true,
            .colon_blink = cfg.colon_blink,
            .color_on    = cfg.color_on,
            .color_off   = cfg.color_off,
            .intensity   = cfg.intensity,
            .anim_effect = wt_led_anim_effect_from_name(cfg.bg_effect_en ? cfg.anim_effect : NULL),
            .led_count   = cfg.led_count,
        };

        if (strcmp(cfg.display_mode, "number") == 0)
        {
            req.mode        = WT_SEGD_MODE_NUMBER;
            req.colon       = false;
            req.colon_blink = false;
        }
        else if (strcmp(cfg.display_mode, "text") == 0)
        {
            req.mode        = WT_SEGD_MODE_TEXT;
            req.colon       = false;
            req.colon_blink = false;
            strlcpy(req.text, cfg.display_text, sizeof(req.text));
        }
        else
        {
            req.mode = WT_SEGD_MODE_TIME;
        }

        bool changed = !has_last_req || !segd_request_equal(&last_req, &req);
        if (wt_segd_queue && (changed || req.mode == WT_SEGD_MODE_TIME))
        {
            xQueueOverwrite(wt_segd_queue, &req);
            last_req     = req;
            has_last_req = true;
        }

        // Save settings to flash once they have stopped changing.
        wt_settings_flush_if_due();

        // Wake at once when settings change; otherwise poll so the clock
        // keeps advancing and a pending save is not left waiting.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WT_MAIN_POLL_MS));
    }
}

/*!
    \brief  Application entry point: initializes the modules, starts the tasks
            and then runs the display-request loop. Never returns.
*/
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    setenv("TZ", "UTC0", 1);
    tzset();

    wt_log_init();

    wt_log_info("========== WATCHTOWER APPLICATION STARTED ==========");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wt_time_init();

    wt_settings_init();

    // Queues must exist before the tasks that use them start.
    wt_segd_queue   = xQueueCreate(1, sizeof(wt_segd_request_t));
    wt_buzzer_queue = xQueueCreate(5, sizeof(wt_sound_event_t));
    wt_segd_snapshot_init();
    if (!wt_segd_queue || !wt_buzzer_queue)
    {
        wt_log_error("Failed to create shared queues rebooting");
        esp_restart();
    }

    BaseType_t task_ok = pdPASS;
    task_ok &= xTaskCreatePinnedToCore(wt_task_wifi, "WT_WIFI", 8192, NULL, 4, NULL, 0);
    task_ok &= xTaskCreatePinnedToCore(wt_task_web, "WT_WEB", 16384, NULL, 3, NULL, 0);

    task_ok &= xTaskCreatePinnedToCore(wt_task_led, "WT_LED", 16384, NULL, 5, NULL, 1);
    task_ok &= xTaskCreatePinnedToCore(wt_task_sound, "WT_SOUND", 4096, NULL, 4, NULL, 1);

    if (task_ok != pdPASS)
    {
        wt_log_error("Failed to create one or more application tasks rebooting");
        esp_restart();
    }

    // The main task runs the display-request loop itself instead of idling.
    // Its stack, core and priority come from CONFIG_ESP_MAIN_TASK_*.
    wt_display_request_loop();
}
