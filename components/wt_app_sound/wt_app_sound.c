//=============================================================================
/*!
    \file    wt_app_sound.c
    \brief   Buzzer task: plays sound events as note sequences.
*/
//=============================================================================

#include "wt_app_sound.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "wt_app_log.h"

#define WT_BUZZER_GPIO    11
#define WT_BUZZER_CHANNEL LEDC_CHANNEL_0
#define WT_BUZZER_TIMER   LEDC_TIMER_0

#define REST 0
#define C4   262
#define D4   294
#define E4   330
#define F4   349
#define G4   392
#define A4   440
#define B4   494
#define C5   523
#define E5   659
#define G5   784

#define MAX_NOTES 10

typedef struct
{
    uint16_t freq;
    uint16_t duration_ms;
} wt_buzzer_note_t;

QueueHandle_t wt_buzzer_queue = NULL;

// Note sequence for each sound event; unused notes are left zero.
static const wt_buzzer_note_t wt_sound[TOTAL_SOUNDS][MAX_NOTES] = {
    [NO_SOUND] =
        {
            {0, 0},
        },

    [NOTIFY_DEFAULT] =
        {
            {C5, 100},
            {REST, 50},
            {C5, 120},
        },

    [WARNING_DEFAULT] =
        {
            {A4, 200},
            {REST, 100},
            {A4, 200},
            {REST, 100},
            {A4, 200},
        },

    [ALARM_DEFAULT] =
        {
            {A4, 400},
            {REST, 200},
            {C5, 400},
            {REST, 200},
        },

    [MELODY_DEFAULT] =
        {
            {C4, 200},
            {D4, 200},
            {E4, 200},
            {C4, 200},
            {E4, 200},
            {F4, 200},
            {G4, 400},
        },

    [SUCCESS_SOUND] =
        {
            {C5, 100},
            {E5, 100},
            {G5, 150},
        },

    [ERROR_SOUND] =
        {
            {G4, 200},
            {REST, 100},
            {G4, 300},
        },

    [FAST_ALARM] =
        {
            {C5, 150},
            {REST, 50},
            {C5, 150},
            {REST, 50},
            {C5, 150},
        },
};

/*!
    \brief  Sound a tone of the given frequency; 0 is silence.
*/
static void buzzer_play_freq(uint32_t freq)
{
    if (freq == 0)
    {
        if (ledc_stop(LEDC_LOW_SPEED_MODE, WT_BUZZER_CHANNEL, 0) != ESP_OK)
        {
            wt_log_warn("ledc_stop failed");
        }
        return;
    }

    if (ledc_set_freq(LEDC_LOW_SPEED_MODE, WT_BUZZER_TIMER, freq) != ESP_OK ||
        ledc_set_duty(LEDC_LOW_SPEED_MODE, WT_BUZZER_CHANNEL, 512) != ESP_OK ||
        ledc_update_duty(LEDC_LOW_SPEED_MODE, WT_BUZZER_CHANNEL) != ESP_OK)
    {
        wt_log_warn("Buzzer freq/duty update failed");
    }
}

/*!
    \brief  Ask the sound task to play a sound. Does not wait.
    \param[in]  event  Sound to play.
*/
void wt_sound_play_event(wt_sound_event_t event)
{
    if (wt_buzzer_queue)
    {
        xQueueSend(wt_buzzer_queue, &event, 0);
    }
}

/*!
    \brief  Sound task: plays each requested sound in turn.
    \param[in]  pvParameter  Unused.
*/
void wt_task_sound(void *pvParameter)
{
    if (!wt_buzzer_queue)
    {
        wt_log_error("wt_buzzer_queue not created before wt_task_sound started");
        vTaskDelete(NULL);
        return;
    }

    // PWM timer and channel that drive the buzzer
    ledc_timer_config_t timer = {.speed_mode      = LEDC_LOW_SPEED_MODE,
                                 .timer_num       = WT_BUZZER_TIMER,
                                 .duty_resolution = LEDC_TIMER_10_BIT,
                                 .freq_hz         = 2000,
                                 .clk_cfg         = LEDC_AUTO_CLK};
    if (ledc_timer_config(&timer) != ESP_OK)
    {
        wt_log_error("ledc_timer_config failed");
    }

    ledc_channel_config_t channel = {.gpio_num   = WT_BUZZER_GPIO,
                                     .speed_mode = LEDC_LOW_SPEED_MODE,
                                     .channel    = WT_BUZZER_CHANNEL,
                                     .timer_sel  = WT_BUZZER_TIMER,
                                     .duty       = 0,
                                     .hpoint     = 0};
    if (ledc_channel_config(&channel) != ESP_OK)
    {
        wt_log_error("ledc_channel_config failed");
    }

    wt_sound_event_t event;

    for (;;)
    {
        if (xQueueReceive(wt_buzzer_queue, &event, portMAX_DELAY))
        {
            if (event >= TOTAL_SOUNDS)
                continue;

            wt_log_info("Sound event: %d", event);

            // Play the notes up to the first zero duration, then go silent.
            const wt_buzzer_note_t *sound = wt_sound[event];
            for (size_t i = 0; i < sizeof(wt_sound[event]) / sizeof(wt_buzzer_note_t); i++)
            {
                if (sound[i].duration_ms == 0)
                    break;
                buzzer_play_freq(sound[i].freq);
                vTaskDelay(pdMS_TO_TICKS(sound[i].duration_ms));
            }
            buzzer_play_freq(0);
        }
    }
}
