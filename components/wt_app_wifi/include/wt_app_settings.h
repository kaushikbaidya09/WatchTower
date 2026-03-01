/*!
    \file   wt_app_settings.h
    \brief  Persistent application settings stored in NVS.

    Settings are loaded at boot and saved whenever wt_settings_set() is called.
    All access is thread-safe.
 */
#ifndef WT_APP_SETTINGS_H
#define WT_APP_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "wt_seg_display.h"

/* ------------------------------------------------------------------ */
/*  Settings structure                                                   */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* Display */
    wt_segd_color_t color_on;  /*!< Segment ON color                */
    wt_segd_color_t color_off; /*!< Segment OFF colour (dim/off)    */
    uint8_t intensity;         /*!< Master brightness 0-255         */
    wt_segd_anim_t anim;       /*!< Animation mode                  */
    bool colon_blink;          /*!< Blink colon at ~1 Hz            */

    /* Time */
    char timezone[48]; /*!< POSIX TZ string e.g "IST-5:30" */
} wt_settings_t;

/* ------------------------------------------------------------------ */
/*  API                                                                  */
/* ------------------------------------------------------------------ */

/** Load from NVS (or defaults if first boot). Call once before tasks start. */
void wt_settings_init(void);

/** Get a copy of the current settings (thread-safe). */
wt_settings_t wt_settings_get(void);

/**
 * @brief  Apply and persist new settings.
 * @return true on success, false if NVS write failed.
 */
bool wt_settings_set(const wt_settings_t *s);

#endif /* WT_APP_SETTINGS_H */
