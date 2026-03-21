/*!
    \file   wt_app_settings.c
    \brief  NVS-backed application settings — expanded for full web console.
 */
#include "wt_app_settings.h"
#include "wt_app_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  NVS Keys                                                          */
/* ------------------------------------------------------------------ */

#define WT_NVS_CONFIG "wt_cfg"
/* display */
#define WT_NVSK_COLOR_HEX "color_hex"
#define WT_NVSK_COL_ON_R "col_r"
#define WT_NVSK_COL_ON_G "col_g"
#define WT_NVSK_COL_ON_B "col_b"
#define WT_NVSK_INTENSITY "intensity"
#define WT_NVSK_ANIM "anim"
#define WT_NVSK_COLON_BLK "colon_blk"
#define WT_NVSK_ANIM_SCROLL "anim_scroll"
#define WT_NVSK_ANIM_PULSE "anim_pulse"
#define WT_NVSK_ANIM_TRANS "anim_trans"
#define WT_NVSK_REACT_EFF "react_eff"
#define WT_NVSK_DISP_MODE "disp_mode"
#define WT_NVSK_DISP_VALUE "disp_value"
#define WT_NVSK_DISP_TEXT "disp_text"
/* clock */
#define WT_NVSK_TIMEZONE "timezone"
#define WT_NVSK_NTP_SRV "ntp_srv"
#define WT_NVSK_TIME_FMT "time_fmt"
#define WT_NVSK_AL1_TIME "al1_time"
#define WT_NVSK_AL1_EN "al1_en"
#define WT_NVSK_AL2_TIME "al2_time"
#define WT_NVSK_AL2_EN "al2_en"
#define WT_NVSK_NOTIF_TYPE "notif_type"
#define WT_NVSK_NOTIF_SND "notif_snd"
/* power */
#define WT_NVSK_SLEEP_MODE "sleep_mode"
#define WT_NVSK_SLEEP_TO "sleep_to"
#define WT_NVSK_BATT_ALERT "batt_alert"
#define WT_NVSK_PS_DIM "ps_dim"
#define WT_NVSK_PS_WIFI "ps_wifi"

#define WT_NVS_GET_UINT8(k, d)                  \
    if (nvs_get_u8(wt_nvs_h, k, &u8) == ESP_OK) \
        (d) = u8;
#define WT_NVS_GET_UINT16(k, d)                   \
    if (nvs_get_u16(wt_nvs_h, k, &u16) == ESP_OK) \
        (d) = u16;
#define WT_NVS_GET_STR(k, d) \
    len = sizeof(d);         \
    nvs_get_str(wt_nvs_h, k, (d), &len);

static wt_settings_t wt_app_setting;
static SemaphoreHandle_t wt_app_setting_mutex = NULL;

/* ------------------------------------------------------------------ */
/*  NVS Default Application Settings                                  */
/* ------------------------------------------------------------------ */

static const wt_settings_t wt_app_default_setting = {
    .color_on = {200, 200, 200},
    .color_off = {0, 0, 0},
    .intensity = 50,
    .anim = WT_SEGD_ANIM_SOLID,
    .colon_blink = true,
    .anim_scroll = false,
    .anim_pulse = false,
    .anim_transition = true,
    .reaction_effect = "none",
    .color_hex = "#00c8ff",
    .display_mode = "time",
    .display_value = 1234,
    .display_text = "HELO",
    .timezone = "IST-5:30",
    .ntp_server = "pool.ntp.org",
    .time_format = 24,
    .alarm1_time = "07:00",
    .alarm1_en = false,
    .alarm2_time = "22:00",
    .alarm2_en = false,
    .notif_type = "flash",
    .notif_sound = "beep",
    .sleep_mode = "none",
    .sleep_timeout_s = 30,
    .batt_alert_pct = 20,
    .ps_dim = true,
    .ps_wifi = false,
};

bool wt_settings_parse_hex_color(const char *hex, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (!hex || hex[0] != '#' || strlen(hex) < 7)
    {
        return false;
    }
    unsigned rv = 0, gv = 0, bv = 0;
    if (sscanf(hex + 1, "%02x%02x%02x", &rv, &gv, &bv) != 3)
    {
        APPLOG_E("Failed to parse hex colors!");
        return false;
    }
    *r = (uint8_t)rv;
    *g = (uint8_t)gv;
    *b = (uint8_t)bv;
    return true;
}

static void load_from_nvs(void)
{
    nvs_handle_t wt_nvs_h;
    if (nvs_open(WT_NVS_CONFIG, NVS_READONLY, &wt_nvs_h) != ESP_OK)
    {
        APPLOG_E("Failed to load settings from NVS!");
        return;
    }

    uint8_t u8 = 0;
    uint16_t u16 = 0;
    size_t len;

    WT_NVS_GET_UINT8(WT_NVSK_COL_ON_R, wt_app_setting.color_on.red)
    WT_NVS_GET_UINT8(WT_NVSK_COL_ON_G, wt_app_setting.color_on.green)
    WT_NVS_GET_UINT8(WT_NVSK_COL_ON_B, wt_app_setting.color_on.blue)
    WT_NVS_GET_UINT8(WT_NVSK_INTENSITY, wt_app_setting.intensity)
    WT_NVS_GET_UINT8(WT_NVSK_ANIM, u8);
    wt_app_setting.anim = (wt_segd_anim_t)u8;
    WT_NVS_GET_UINT8(WT_NVSK_COLON_BLK, u8);
    wt_app_setting.colon_blink = (bool)u8;
    WT_NVS_GET_UINT8(WT_NVSK_ANIM_SCROLL, u8);
    wt_app_setting.anim_scroll = (bool)u8;
    WT_NVS_GET_UINT8(WT_NVSK_ANIM_PULSE, u8);
    wt_app_setting.anim_pulse = (bool)u8;
    WT_NVS_GET_UINT8(WT_NVSK_ANIM_TRANS, u8);
    wt_app_setting.anim_transition = (bool)u8;
    WT_NVS_GET_STR(WT_NVSK_REACT_EFF, wt_app_setting.reaction_effect)
    WT_NVS_GET_STR(WT_NVSK_COLOR_HEX, wt_app_setting.color_hex)
    WT_NVS_GET_STR(WT_NVSK_DISP_MODE, wt_app_setting.display_mode)
    nvs_get_i16(wt_nvs_h, WT_NVSK_DISP_VALUE, &wt_app_setting.display_value);
    WT_NVS_GET_STR(WT_NVSK_DISP_TEXT, wt_app_setting.display_text)
    WT_NVS_GET_STR(WT_NVSK_TIMEZONE, wt_app_setting.timezone)
    WT_NVS_GET_STR(WT_NVSK_NTP_SRV, wt_app_setting.ntp_server)
    WT_NVS_GET_UINT8(WT_NVSK_TIME_FMT, wt_app_setting.time_format)
    WT_NVS_GET_STR(WT_NVSK_AL1_TIME, wt_app_setting.alarm1_time)
    WT_NVS_GET_UINT8(WT_NVSK_AL1_EN, u8);
    wt_app_setting.alarm1_en = (bool)u8;
    WT_NVS_GET_STR(WT_NVSK_AL2_TIME, wt_app_setting.alarm2_time)
    WT_NVS_GET_UINT8(WT_NVSK_AL2_EN, u8);
    wt_app_setting.alarm2_en = (bool)u8;
    WT_NVS_GET_STR(WT_NVSK_NOTIF_TYPE, wt_app_setting.notif_type)
    WT_NVS_GET_STR(WT_NVSK_NOTIF_SND, wt_app_setting.notif_sound)
    WT_NVS_GET_STR(WT_NVSK_SLEEP_MODE, wt_app_setting.sleep_mode)
    WT_NVS_GET_UINT16(WT_NVSK_SLEEP_TO, wt_app_setting.sleep_timeout_s)
    WT_NVS_GET_UINT8(WT_NVSK_BATT_ALERT, wt_app_setting.batt_alert_pct)
    WT_NVS_GET_UINT8(WT_NVSK_PS_DIM, u8);
    wt_app_setting.ps_dim = (bool)u8;
    WT_NVS_GET_UINT8(WT_NVSK_PS_WIFI, u8);
    wt_app_setting.ps_wifi = (bool)u8;

    nvs_close(wt_nvs_h);
}

static bool save_to_nvs(const wt_settings_t *s)
{
    nvs_handle_t wt_nvs_h;
    if (nvs_open(WT_NVS_CONFIG, NVS_READWRITE, &wt_nvs_h) != ESP_OK)
    {
        APPLOG_E("Failed to open NVS read-write!");
        return false;
    }

    nvs_set_u8(wt_nvs_h, WT_NVSK_COL_ON_R, s->color_on.red);
    nvs_set_u8(wt_nvs_h, WT_NVSK_COL_ON_G, s->color_on.green);
    nvs_set_u8(wt_nvs_h, WT_NVSK_COL_ON_B, s->color_on.blue);
    nvs_set_u8(wt_nvs_h, WT_NVSK_INTENSITY, s->intensity);
    nvs_set_u8(wt_nvs_h, WT_NVSK_ANIM, (uint8_t)s->anim);
    nvs_set_u8(wt_nvs_h, WT_NVSK_COLON_BLK, (uint8_t)s->colon_blink);
    nvs_set_u8(wt_nvs_h, WT_NVSK_ANIM_SCROLL, (uint8_t)s->anim_scroll);
    nvs_set_u8(wt_nvs_h, WT_NVSK_ANIM_PULSE, (uint8_t)s->anim_pulse);
    nvs_set_u8(wt_nvs_h, WT_NVSK_ANIM_TRANS, (uint8_t)s->anim_transition);
    nvs_set_str(wt_nvs_h, WT_NVSK_REACT_EFF, s->reaction_effect);
    nvs_set_str(wt_nvs_h, WT_NVSK_COLOR_HEX, s->color_hex);
    nvs_set_str(wt_nvs_h, WT_NVSK_DISP_MODE, s->display_mode);
    nvs_set_i16(wt_nvs_h, WT_NVSK_DISP_VALUE, s->display_value);
    nvs_set_str(wt_nvs_h, WT_NVSK_DISP_TEXT, s->display_text);
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
        APPLOG_E("Failed to commit NVS!");
    }
    nvs_close(wt_nvs_h);
    return (err == ESP_OK);
}

static void normalize_settings(wt_settings_t *s)
{
    if (!s)
    {
        return;
    }

    if (strcmp(s->reaction_effect, "rainbow") == 0)
    {
        s->anim = WT_SEGD_ANIM_RAINBOW;
    }
    else if (s->anim_pulse)
    {
        s->anim = WT_SEGD_ANIM_PULSE;
    }
    else
    {
        s->anim = WT_SEGD_ANIM_SOLID;
    }

    if ((strcmp(s->display_mode, "time") != 0) &&
        (strcmp(s->display_mode, "number") != 0) &&
        (strcmp(s->display_mode, "text") != 0))
    {
        strlcpy(s->display_mode, "time", sizeof(s->display_mode));
    }

    if (s->display_value < 0)
    {
        s->display_value = 0;
    }
    if (s->display_value > 9999)
    {
        s->display_value = 9999;
    }
}

void wt_settings_init(void)
{
    wt_app_setting_mutex = xSemaphoreCreateMutex();
    wt_app_setting = wt_app_default_setting;
    load_from_nvs();
    wt_settings_parse_hex_color(wt_app_setting.color_hex,
                                &wt_app_setting.color_on.red,
                                &wt_app_setting.color_on.green,
                                &wt_app_setting.color_on.blue);
    normalize_settings(&wt_app_setting);
    setenv("TZ", wt_app_setting.timezone, 1);
    tzset();
    APPLOG_I("Settings loaded (tz=%s fmt=%dh color=%s)",
             wt_app_setting.timezone, wt_app_setting.time_format, wt_app_setting.color_hex);
}

wt_settings_t wt_settings_get(void)
{
    wt_settings_t snap;

    xSemaphoreTake(wt_app_setting_mutex, portMAX_DELAY);
    snap = wt_app_setting;
    xSemaphoreGive(wt_app_setting_mutex);

    return snap;
}

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
    xSemaphoreGive(wt_app_setting_mutex);

    setenv("TZ", normalized.timezone, 1);
    tzset();

    bool status = save_to_nvs(&normalized);
    if (status != true)
    {
        APPLOG_E("Failed to save settings!");
    }

    return status;
}
