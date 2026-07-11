#ifndef WT_APP_LED_H
#define WT_APP_LED_H

#include <stdint.h>

void wt_task_led(void *pvParameter);

/*!
    \brief  Map a demo-effect name (e.g. "ripple") to the run_effects() mode
            integer used in wt_segd_request_t.demo_effect. Unknown/NULL
            names return the rainbow_ring mode (0).
 */
uint8_t wt_led_demo_effect_from_name(const char *name);

#endif // WT_APP_LED_H