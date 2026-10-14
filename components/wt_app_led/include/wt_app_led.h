//=============================================================================
/*!
    \file    wt_app_led.h
    \brief   LED display task.
*/
//=============================================================================

#ifndef WT_APP_LED_H
#define WT_APP_LED_H

#include <stdint.h>

void wt_task_led(void *pvParameter);

uint8_t wt_led_anim_effect_from_name(const char *name);

#endif /* WT_APP_LED_H */
