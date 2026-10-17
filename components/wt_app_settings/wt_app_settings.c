//=============================================================================
/*!
    \file    wt_app_settings.c
    \brief   User settings, saved in NVS.
*/
//=============================================================================

// Settings are held in RAM and saved to NVS after a change;
// normalize_settings() keeps every field within its valid range.

#include "wt_app_settings.h"
#include "wt_app_log.h"
#include "wt_app_led.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define WT_NVS_CONFIG "wt_cfg"
// NVS keys: display
#define WT_NVSK_COLOR_HEX   "color_hex"
#define WT_NVSK_COL_ON_R    "col_r"
#define WT_NVSK_COL_ON_G    "col_g"
#define WT_NVSK_COL_ON_B    "col_b"
#define WT_NVSK_INTENSITY   "intensity"
#define WT_NVSK_COLON_BLK   "colon_blk"
#define WT_NVSK_ANIM_SCROLL "anim_scroll"
#define WT_NVSK_ANIM_TRANS  "anim_trans"
#define WT_NVSK_DISP_MODE   "disp_mode"
#define WT_NVSK_DISP_VALUE  "disp_value"
#define WT_NVSK_DISP_TEXT   "disp_text"
#define WT_NVSK_BG_FX_EN    "bg_fx_en"
#define WT_NVSK_ANIM_EFFECT "anim_fx"
#define WT_NVSK_LED_COUNT   "led_count"
// NVS keys: clock
#define WT_NVSK_TIMEZONE   "timezone"
#define WT_NVSK_NTP_SRV    "ntp_srv"
#define WT_NVSK_TIME_FMT   "time_fmt"
#define WT_NVSK_AL1_TIME   "al1_time"
#define WT_NVSK_AL1_EN     "al1_en"
#define WT_NVSK_AL2_TIME   "al2_time"
#define WT_NVSK_AL2_EN     "al2_en"
#define WT_NVSK_NOTIF_TYPE "notif_type"
#define WT_NVSK_NOTIF_SND  "notif_snd"
// NVS keys: power
#define WT_NVSK_SLEEP_MODE "sleep_mode"
#define WT_NVSK_SLEEP_TO   "sleep_to"
#define WT_NVSK_BATT_ALERT "batt_alert"
#define WT_NVSK_PS_DIM     "ps_dim"
#define WT_NVSK_PS_WIFI    "ps_wifi"

#define WT_NVS_GET_UINT8(k, d)                                                                     \
    if (nvs_get_u8(wt_nvs_h, k, &u8) == ESP_OK)                                                    \
        (d) = u8;
#define WT_NVS_GET_UINT16(k, d)                                                                    \
    if (nvs_get_u16(wt_nvs_h, k, &u16) == ESP_OK)                                                  \
        (d) = u16;
#define WT_NVS_GET_STR(k, d)                                                                       \
    len = sizeof(d);                                                                               \
    nvs_get_str(wt_nvs_h, k, (d), &len);
// Reads a bool stored as u8. A key missing from NVS keeps its default.
#define WT_NVS_GET_BOOL(k, d)                                                                      \
    if (nvs_get_u8(wt_nvs_h, k, &u8) == ESP_OK)                                                    \
        (d) = (u8 != 0);

static wt_settings_t wt_app_setting;
static SemaphoreHandle_t wt_app_setting_mutex = NULL;
static TaskHandle_t s_notify_task             = NULL;
static uint32_t s_generation = 0;         ///< Increases on every change; protected by the mutex
static bool s_dirty          = false;     ///< Changes not yet saved to NVS; protected by the mutex
static TickType_t s_last_change_tick = 0; ///< Tick count of the last change; protected by the mutex

#define WT_SETTINGS_FLUSH_DELAY_MS 1500 ///< Delay after the last change before saving

static const wt_settings_t wt_app_default_setting = {
    .color_on        = {200, 200, 200},
    .color_off       = {0, 0, 0},
    .intensity       = 64, // 25%
    .colon_blink     = true,
    .anim_scroll     = false,
    .anim_transition = true,
    .color_hex       = "#00c8ff",
    .display_mode    = "time",
    .display_value   = 1234,
    .display_text    = "HELO",
    .bg_effect_en    = false,
    .anim_effect     = "rainbow_ring",
    .led_count       = WT_SEGD_TOTAL_LEDS,
    .timezone        = "IST-5:30",
    .ntp_server      = "pool.ntp.org",
    .time_format     = 24,
    .alarm1_time     = "07:00",
    .alarm1_en       = false,
    .alarm2_time     = "22:00",
    .alarm2_en       = false,
    .notif_type      = "flash",
    .notif_sound     = "beep",
    .sleep_mode      = "none",
    .sleep_timeout_s = 30,
    .batt_alert_pct  = 20,
    .ps_dim          = true,
    .ps_wifi         = false,
};

/*!
    \brief  Convert a "#rrggbb" color string to red, green and blue values.
    \param[in]  hex  Color string, e.g. "#ff8800".
    \param[out] r    Receives red.
    \param[out] g    Receives green.
    \param[out] b    Receives blue.
    \return true if the string was a valid color.
*/
bool wt_settings_parse_hex_color(const char *hex, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!hex || hex[0] != '#' || strlen(hex) < 7)
    {
        return false;
    }
    unsigned rv = 0, gv = 0, bv = 0;
    if (!r || !g || !b)
    {
        return false;
    }
    if (sscanf(hex + 1, "%02x%02x%02x", &rv, &gv, &bv) != 3)
    {
        wt_log_error("Failed to parse hex colors!");
        return false;
    }
    *r = (uint8_t)rv;
    *g = (uint8_t)gv;
    *b = (uint8_t)bv;
    return true;
}

/*!
    \brief  Load the saved settings. A setting that was never saved keeps
            its default.
*/
static void load_from_nvs(void)
{
    nvs_handle_t wt_nvs_h;
    if (nvs_open(WT_NVS_CONFIG, NVS_READONLY, &wt_nvs_h) != ESP_OK)
    {
        wt_log_error("Failed to load settings from NVS!");
        return;
    }

    uint8_t u8   = 0;
    uint16_t u16 = 0;
    size_t len;

    WT_NVS_GET_UINT8(WT_NVSK_COL_ON_R, wt_app_setting.color_on.red)
    WT_NVS_GET_UINT8(WT_NVSK_COL_ON_G, wt_app_setting.color_on.green)
    WT_NVS_GET_UINT8(WT_NVSK_COL_ON_B, wt_app_setting.color_on.blue)
    WT_NVS_GET_UINT8(WT_NVSK_INTENSITY, wt_app_setting.intensity)
    WT_NVS_GET_BOOL(WT_NVSK_COLON_BLK, wt_app_setting.colon_blink)
    WT_NVS_GET_BOOL(WT_NVSK_ANIM_SCROLL, wt_app_setting.anim_scroll)
    WT_NVS_GET_BOOL(WT_NVSK_ANIM_TRANS, wt_app_setting.anim_transition)
    WT_NVS_GET_STR(WT_NVSK_COLOR_HEX, wt_app_setting.color_hex)
    WT_NVS_GET_STR(WT_NVSK_DISP_MODE, wt_app_setting.display_mode)
    nvs_get_i16(wt_nvs_h, WT_NVSK_DISP_VALUE, &wt_app_setting.display_value);
    WT_NVS_GET_STR(WT_NVSK_DISP_TEXT, wt_app_setting.display_text)
    WT_NVS_GET_BOOL(WT_NVSK_BG_FX_EN, wt_app_setting.bg_effect_en)
    WT_NVS_GET_STR(WT_NVSK_ANIM_EFFECT, wt_app_setting.anim_effect)
    WT_NVS_GET_UINT16(WT_NVSK_LED_COUNT, wt_app_setting.led_count)
    WT_NVS_GET_STR(WT_NVSK_TIMEZONE, wt_app_setting.timezone)
    WT_NVS_GET_STR(WT_NVSK_NTP_SRV, wt_app_setting.ntp_server)
    WT_NVS_GET_UINT8(WT_NVSK_TIME_FMT, wt_app_setting.time_format)
    WT_NVS_GET_STR(WT_NVSK_AL1_TIME, wt_app_setting.alarm1_time)
    WT_NVS_GET_BOOL(WT_NVSK_AL1_EN, wt_app_setting.alarm1_en)
    WT_NVS_GET_STR(WT_NVSK_AL2_TIME, wt_app_setting.alarm2_time)
    WT_NVS_GET_BOOL(WT_NVSK_AL2_EN, wt_app_setting.alarm2_en)
    WT_NVS_GET_STR(WT_NVSK_NOTIF_TYPE, wt_app_setting.notif_type)
    WT_NVS_GET_STR(WT_NVSK_NOTIF_SND, wt_app_setting.notif_sound)
    WT_NVS_GET_STR(WT_NVSK_SLEEP_MODE, wt_app_setting.sleep_mode)
    WT_NVS_GET_UINT16(WT_NVSK_SLEEP_TO, wt_app_setting.sleep_timeout_s)
    WT_NVS_GET_UINT8(WT_NVSK_BATT_ALERT, wt_app_setting.batt_alert_pct)
    WT_NVS_GET_BOOL(WT_NVSK_PS_DIM, wt_app_setting.ps_dim)
    WT_NVS_GET_BOOL(WT_NVSK_PS_WIFI, wt_app_setting.ps_wifi)

    nvs_close(wt_nvs_h);
}

/*!
    \brief  Save all settings to NVS.
*/
static bool save_to_nvs(const wt_settings_t *s)
{
    nvs_handle_t wt_nvs_h;
    if (nvs_open(WT_NVS_CONFIG, NVS_READWRITE, &wt_nvs_h) != ESP_OK)
    {
        wt_log_error("Failed to open NVS read-write!");
        return false;
    }

    nvs_set_u8(wt_nvs_h, WT_NVSK_COL_ON_R, s->color_on.red);
    nvs_set_u8(wt_nvs_h, WT_NVSK_COL_ON_G, s->color_on.green);
    nvs_set_u8(wt_nvs_h, WT_NVSK_COL_ON_B, s->color_on.blue);
    nvs_set_u8(wt_nvs_h, WT_NVSK_INTENSITY, s->intensity);
    nvs_set_u8(wt_nvs_h, WT_NVSK_COLON_BLK, (uint8_t)s->colon_blink);
    nvs_set_u8(wt_nvs_h, WT_NVSK_ANIM_SCROLL, (uint8_t)s->anim_scroll);
    nvs_set_u8(wt_nvs_h, WT_NVSK_ANIM_TRANS, (uint8_t)s->anim_transition);
    nvs_set_str(wt_nvs_h, WT_NVSK_COLOR_HEX, s->color_hex);
    nvs_set_str(wt_nvs_h, WT_NVSK_DISP_MODE, s->display_mode);
    nvs_set_i16(wt_nvs_h, WT_NVSK_DISP_VALUE, s->display_value);
    nvs_set_str(wt_nvs_h, WT_NVSK_DISP_TEXT, s->display_text);
    nvs_set_u8(wt_nvs_h, WT_NVSK_BG_FX_EN, (uint8_t)s->bg_effect_en);
    nvs_set_str(wt_nvs_h, WT_NVSK_ANIM_EFFECT, s->anim_effect);
    nvs_set_u16(wt_nvs_h, WT_NVSK_LED_COUNT, s->led_count);
    nvs_set_str(wt_nvs_h, WT_NVSK_TIMEZONE, s->timezone);
    nvs_set_str(wt_nvs_h, WT_NVSK_NTP_SRV, s->ntp_server);
    nvs_set_u8(wt_nvs_h, WT_NVSK_TIME_FMT, s->time_format);
    nvs_set_str(wt_nvs_h, WT_NVSK_AL1_TIME, s->alarm1_time);
    nvs_set_u8(wt_nvs_h, WT_NVSK_AL1_EN, (uint8_t)s->alarm1_en);
    nvs_set_str(wt_nvs_h, WT_NVSK_AL2_TIME, s->alarm2_time);
    nvs_set_u8(wt_nvs_h, WT_NVSK_AL2_EN, (uint8_t)s->alarm2_en);
    nvs_set_str(wt_nvs_h, WT_NVSK_NOTIF_TYPE, s->notif_type);
    nvs_set_str(wt_nvs_h, WT_NVSK_NOTIF_SND, s->notif_sound);
    nvs_set_str(wt_nvs_h, WT_NVSK_SLEEP_MODE, s->sleep_mode);
    nvs_set_u16(wt_nvs_h, WT_NVSK_SLEEP_TO, s->sleep_timeout_s);
    nvs_set_u8(wt_nvs_h, WT_NVSK_BATT_ALERT, s->batt_alert_pct);
    nvs_set_u8(wt_nvs_h, WT_NVSK_PS_DIM, (uint8_t)s->ps_dim);
    nvs_set_u8(wt_nvs_h, WT_NVSK_PS_WIFI, (uint8_t)s->ps_wifi);

    esp_err_t err = nvs_commit(wt_nvs_h);
    if (err != ESP_OK)
    {
        wt_log_error("Failed to commit NVS!");
    }
    nvs_close(wt_nvs_h);
    return (err == ESP_OK);
}

/*!
    \brief  Bring every setting into its valid range.
*/
static void normalize_settings(wt_settings_t *s)
{
    if (!s)
    {
        return;
    }

    if ((strcmp(s->display_mode, "time") != 0) && (strcmp(s->display_mode, "number") != 0) &&
        (strcmp(s->display_mode, "text") != 0))
    {
        strlcpy(s->display_mode, "time", sizeof(s->display_mode));
    }

    // An unknown name maps to "solid", the same as no effect; the web
    // console never selects "solid", so it falls back to the default.
    if (wt_led_anim_effect_from_name(s->anim_effect) == wt_led_anim_effect_from_name(NULL))
    {
        strlcpy(s->anim_effect, "rainbow_ring", sizeof(s->anim_effect));
    }

    if (s->display_value < 0)
    {
        s->display_value = 0;
    }
    if (s->display_value > 9999)
    {
        s->display_value = 9999;
    }

    // Only 12- and 24-hour formats exist; anything else becomes 24.
    if (s->time_format != 12)
    {
        s->time_format = 24;
    }

    // An empty time zone or NTP server falls back to the default.
    if (s->timezone[0] == '\0')
    {
        strlcpy(s->timezone, wt_app_default_setting.timezone, sizeof(s->timezone));
    }
    if (s->ntp_server[0] == '\0')
    {
        strlcpy(s->ntp_server, wt_app_default_setting.ntp_server, sizeof(s->ntp_server));
    }

    if (s->led_count < WT_SEGD_TOTAL_LEDS)
    {
        s->led_count = WT_SEGD_TOTAL_LEDS;
    }
    if (s->led_count > WT_SEGD_MAX_TOTAL_LEDS)
    {
        s->led_count = WT_SEGD_MAX_TOTAL_LEDS;
    }
}

/*!
    \brief  Load the settings and apply the time zone.
*/
void wt_settings_init(void)
{
    wt_app_setting_mutex = xSemaphoreCreateMutex();
    wt_app_setting       = wt_app_default_setting;
    load_from_nvs();
    wt_settings_parse_hex_color(wt_app_setting.color_hex, &wt_app_setting.color_on.red,
                                &wt_app_setting.color_on.green, &wt_app_setting.color_on.blue);
    normalize_settings(&wt_app_setting);
    setenv("TZ", wt_app_setting.timezone, 1);
    tzset();
    wt_log_info("Settings loaded (tz=%s fmt=%dh color=%s)", wt_app_setting.timezone,
                wt_app_setting.time_format, wt_app_setting.color_hex);
}

/*!
    \brief  Get a copy of the current settings.
    \return The current settings.
*/
wt_settings_t wt_settings_get(void)
{
    wt_settings_t snap;

    xSemaphoreTake(wt_app_setting_mutex, portMAX_DELAY);
    snap = wt_app_setting;
    xSemaphoreGive(wt_app_setting_mutex);

    return snap;
}

/*!
    \brief  Apply new settings at once. They are saved to NVS later, by
            wt_settings_flush_if_due() or wt_settings_flush().
    \param[in]  wt_settings  New settings.
    \return true if the settings were applied.
*/
bool wt_settings_set(const wt_settings_t *wt_settings)
{
    if (!wt_settings)
    {
        return false;
    }

    wt_settings_t normalized = *wt_settings;
    normalize_settings(&normalized);

    xSemaphoreTake(wt_app_setting_mutex, portMAX_DELAY);
    wt_app_setting = normalized;
    s_generation++;
    s_dirty            = true;
    s_last_change_tick = xTaskGetTickCount();
    xSemaphoreGive(wt_app_setting_mutex);

    setenv("TZ", normalized.timezone, 1);
    tzset();

    // Saving to NVS is deferred (wt_settings_flush_if_due()) so the display
    // reacts immediately.
    if (s_notify_task)
    {
        xTaskNotifyGive(s_notify_task);
    }

    return true;
}

/*!
    \brief  Save the settings to NVS now if they have changed.
    \return true if nothing needed saving or the save succeeded.
*/
bool wt_settings_flush(void)
{
    wt_settings_t snap;
    bool dirty;

    xSemaphoreTake(wt_app_setting_mutex, portMAX_DELAY);
    dirty   = s_dirty;
    snap    = wt_app_setting;
    s_dirty = false;
    xSemaphoreGive(wt_app_setting_mutex);

    if (!dirty)
    {
        return true;
    }

    bool ok = save_to_nvs(&snap);
    if (!ok)
    {
        wt_log_error("Failed to save settings!");

        // Still unsaved: try again on the next flush.
        xSemaphoreTake(wt_app_setting_mutex, portMAX_DELAY);
        s_dirty            = true;
        s_last_change_tick = xTaskGetTickCount();
        xSemaphoreGive(wt_app_setting_mutex);
    }
    return ok;
}

/*!
    \brief  Save the settings once they have stopped changing for a moment.
            Call it regularly.
*/
void wt_settings_flush_if_due(void)
{
    bool due;

    xSemaphoreTake(wt_app_setting_mutex, portMAX_DELAY);
    due = s_dirty &&
          ((xTaskGetTickCount() - s_last_change_tick) >= pdMS_TO_TICKS(WT_SETTINGS_FLUSH_DELAY_MS));
    xSemaphoreGive(wt_app_setting_mutex);

    if (due)
    {
        wt_settings_flush();
    }
}

/*!
    \brief  Choose a task to be woken whenever the settings change.
    \param[in]  task  Task to notify.
*/
void wt_settings_register_notify_task(TaskHandle_t task)
{
    s_notify_task = task;
}

/*!
    \brief  Counter that increases every time the settings change.
*/
uint32_t wt_settings_get_generation(void)
{
    uint32_t gen;

    xSemaphoreTake(wt_app_setting_mutex, portMAX_DELAY);
    gen = s_generation;
    xSemaphoreGive(wt_app_setting_mutex);

    return gen;
}
