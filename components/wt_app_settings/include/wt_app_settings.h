//=============================================================================
/*!
    \file    wt_app_settings.h
    \brief   User settings, saved in NVS.
*/
//=============================================================================

// Thread-safe. A change applies at once; saving it to NVS is deferred
// until the settings stop changing (wt_settings_flush_if_due()).

#ifndef WT_APP_SETTINGS_H
#define WT_APP_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wt_seg_display.h"

typedef struct
{
    // Display
    wt_segd_color_t color_on;
    wt_segd_color_t color_off;
    uint8_t intensity;
    bool colon_blink;
    bool anim_scroll;
    bool anim_transition;
    char color_hex[8]; ///< "#rrggbb"
    char display_mode[8];
    int16_t display_value;
    char display_text[5];
    bool bg_effect_en;    ///< Play the background effect
    char anim_effect[16]; ///< Background effect name
    uint16_t led_count;   ///< LEDs on the strip, 58-100

    // Clock
    char timezone[48];
    char ntp_server[64];
    uint8_t time_format; ///< 12 or 24
    char alarm1_time[6]; ///< "HH:MM"
    bool alarm1_en;
    char alarm2_time[6];
    bool alarm2_en;
    char notif_type[8];
    char notif_sound[8];

    // Power
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

void wt_settings_flush_if_due(void);
bool wt_settings_flush(void);

#endif /* WT_APP_SETTINGS_H */
