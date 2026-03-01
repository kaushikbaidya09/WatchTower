/*!
    \file   wt_app_settings.c
    \brief  NVS-backed application settings with thread-safe access.
 */
#include "wt_app_settings.h"
#include "wt_app_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  NVS namespace / keys                                                 */
/* ------------------------------------------------------------------ */
#define NS "wt_settings"
#define K_COL_ON_R "col_on_r"
#define K_COL_ON_G "col_on_g"
#define K_COL_ON_B "col_on_b"
#define K_COL_OFF_R "col_off_r"
#define K_COL_OFF_G "col_off_g"
#define K_COL_OFF_B "col_off_b"
#define K_INTENSITY "intensity"
#define K_ANIM "anim"
#define K_COLON_BLK "colon_blk"
#define K_TIMEZONE "timezone"

/* ------------------------------------------------------------------ */
/*  Module state                                                         */
/* ------------------------------------------------------------------ */
static wt_settings_t s_settings;
static SemaphoreHandle_t s_mutex = NULL;

/* ------------------------------------------------------------------ */
/*  Defaults                                                             */
/* ------------------------------------------------------------------ */
static const wt_settings_t k_defaults = {
    .color_on = {0, 255, 255}, /* cyan  */
    .color_off = {0, 0, 0},    /* off   */
    .intensity = 200,
    .anim = WT_SEGD_ANIM_SOLID,
    .colon_blink = true,
    .timezone = "IST-5:30",
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                              */
/* ------------------------------------------------------------------ */

static void load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK)
        return;

    uint8_t u8 = 0;
    uint8_t i8 = 0;

#define NVS_GET_U8(key, dst)               \
    if (nvs_get_u8(h, key, &u8) == ESP_OK) \
        (dst) = u8;

    NVS_GET_U8(K_COL_ON_R, s_settings.color_on.r);
    NVS_GET_U8(K_COL_ON_G, s_settings.color_on.g);
    NVS_GET_U8(K_COL_ON_B, s_settings.color_on.b);
    NVS_GET_U8(K_COL_OFF_R, s_settings.color_off.r);
    NVS_GET_U8(K_COL_OFF_G, s_settings.color_off.g);
    NVS_GET_U8(K_COL_OFF_B, s_settings.color_off.b);
    NVS_GET_U8(K_INTENSITY, s_settings.intensity);
    NVS_GET_U8(K_COLON_BLK, i8);
    s_settings.colon_blink = (bool)i8;

    uint8_t anim = 0;
    if (nvs_get_u8(h, K_ANIM, &anim) == ESP_OK)
        s_settings.anim = (wt_segd_anim_t)anim;

    size_t len = sizeof(s_settings.timezone);
    nvs_get_str(h, K_TIMEZONE, s_settings.timezone, &len);

    nvs_close(h);
}

static bool save_to_nvs(const wt_settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK)
        return false;

    nvs_set_u8(h, K_COL_ON_R, s->color_on.r);
    nvs_set_u8(h, K_COL_ON_G, s->color_on.g);
    nvs_set_u8(h, K_COL_ON_B, s->color_on.b);
    nvs_set_u8(h, K_COL_OFF_R, s->color_off.r);
    nvs_set_u8(h, K_COL_OFF_G, s->color_off.g);
    nvs_set_u8(h, K_COL_OFF_B, s->color_off.b);
    nvs_set_u8(h, K_INTENSITY, s->intensity);
    nvs_set_u8(h, K_ANIM, (uint8_t)s->anim);
    nvs_set_u8(h, K_COLON_BLK, (uint8_t)s->colon_blink);
    nvs_set_str(h, K_TIMEZONE, s->timezone);

    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                           */
/* ------------------------------------------------------------------ */

void wt_settings_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_settings = k_defaults;
    load_from_nvs();

    /* Apply timezone immediately */
    setenv("TZ", s_settings.timezone, 1);
    tzset();

    APPLOG_I("Settings loaded (anim=%d intensity=%d tz=%s)",
             s_settings.anim, s_settings.intensity, s_settings.timezone);
}

wt_settings_t wt_settings_get(void)
{
    wt_settings_t snap;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snap = s_settings;
    xSemaphoreGive(s_mutex);
    return snap;
}

bool wt_settings_set(const wt_settings_t *s)
{
    if (!s)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_settings = *s;
    xSemaphoreGive(s_mutex);

    setenv("TZ", s->timezone, 1);
    tzset();

    bool ok = save_to_nvs(s);
    APPLOG_I("Settings saved (ok=%d)", ok);
    return ok;
}
