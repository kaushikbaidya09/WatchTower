/*!
    \file   wt_app_settings.h
    \brief  Persistent application settings stored in NVS.

    \details
    All access is thread-safe via an internal mutex.
 */

#ifndef WT_APP_SETTINGS_H
#define WT_APP_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wt_seg_display.h"

typedef struct
{
    /* Display */
    wt_segd_color_t color_on;
    wt_segd_color_t color_off;
    uint8_t intensity;
    wt_segd_anim_t anim;
    bool colon_blink;
    bool anim_scroll;
    bool anim_pulse;
    bool anim_transition;
    char reaction_effect[16];
    char color_hex[8]; ///< "#rrggbb"
    char display_mode[8];
    int16_t display_value;
    char display_text[5];
    bool bg_effect_en;     ///< Overlay the full-strip anim_effect pattern on top of the current display_mode
    char anim_effect[16]; ///< Which wt_led_anim_effect_from_name() pattern bg_effect_en plays
    uint16_t led_count;    ///< Physical LED count to drive; clamped to [WT_SEGD_TOTAL_LEDS, WT_SEGD_MAX_TOTAL_LEDS]

    /* Time / Clock */
    char timezone[48];
    char ntp_server[64];
    uint8_t time_format; ///< 12 or 24
    char alarm1_time[6]; ///< "HH:MM"
    bool alarm1_en;
    char alarm2_time[6];
    bool alarm2_en;
    char notif_type[8];
    char notif_sound[8];

    /* Power */
    char sleep_mode[8];
    uint16_t sleep_timeout_s;
    uint8_t batt_alert_pct;
    bool ps_dim;
    bool ps_wifi;

} wt_settings_t;

void wt_settings_init(void);
wt_settings_t wt_settings_get(void);
bool wt_settings_set(const wt_settings_t *s);
bool wt_settings_parse_hex_color(const char *hex, uint8_t *r, uint8_t *g, uint8_t *b);

void wt_settings_register_notify_task(TaskHandle_t task);

uint32_t wt_settings_get_generation(void);

#endif /* WT_APP_SETTINGS_H */
