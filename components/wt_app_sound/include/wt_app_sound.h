#ifndef WT_APP_SOUND_H
#define WT_APP_SOUND_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum
{
    NO_SOUND = 0,
    NOTIFY_DEFAULT,
    WARNING_DEFAULT,
    ALARM_DEFAULT,
    MELODY_DEFAULT,
    SUCCESS_SOUND,
    ERROR_SOUND,
    FAST_ALARM,
    TOTAL_SOUNDS
} wt_sound_event_t;

void wt_sound_play_event(wt_sound_event_t event);

void wt_task_sound(void *pvParameter);

extern QueueHandle_t wt_buzzer_queue;

#endif // WT_APP_SOUND_H