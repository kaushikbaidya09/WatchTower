#include "wt_app_sound.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/ledc.h"
#include "wt_app_log.h"

#define WT_BUZZER_GPIO 11
#define WT_BUZZER_CHANNEL LEDC_CHANNEL_0
#define WT_BUZZER_TIMER LEDC_TIMER_0

#define REST 0
#define C4 262
#define D4 294
#define E4 330
#define F4 349
#define G4 392
#define A4 440
#define B4 494
#define C5 523
#define E5 659
#define G5 784

#define MAX_NOTES 10

typedef struct
{
    uint16_t freq;
    uint16_t duration_ms;
} wt_buzzer_note_t;

static QueueHandle_t wt_buzzer_queue;

/* ------------------------------------------------------------------ */
/*  Sound definitions                                                 */
/* ------------------------------------------------------------------ */

/* 2D sound table */
static wt_buzzer_note_t wt_sound[TOTAL_SOUNDS][MAX_NOTES] = {
    [NO_SOUND] = {
        {0, 0},
    },

    [NOTIFY_DEFAULT] = {
        {C5, 100},
        {REST, 50},
        {C5, 120},
    },

    [WARNING_DEFAULT] = {
        {A4, 200},
        {REST, 100},
        {A4, 200},
        {REST, 100},
        {A4, 200},
    },

    [ALARM_DEFAULT] = {
        {A4, 400},
        {REST, 200},
        {C5, 400},
        {REST, 200},
    },

    [MELODY_DEFAULT] = {
        {C4, 200},
        {D4, 200},
        {E4, 200},
        {C4, 200},
        {E4, 200},
        {F4, 200},
        {G4, 400},
    },

    [SUCCESS_SOUND] = {
        {C5, 100},
        {E5, 100},
        {G5, 150},
    },

    [ERROR_SOUND] = {
        {G4, 200},
        {REST, 100},
        {G4, 300},
    },

    [FAST_ALARM] = {
        {C5, 150},
        {REST, 50},
        {C5, 150},
        {REST, 50},
        {C5, 150},
    },
};

/* ------------------------------------------------------------------ */
/*  Buzzer Driver                                                     */
/* ------------------------------------------------------------------ */

static void buzzer_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = WT_BUZZER_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .gpio_num = WT_BUZZER_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = WT_BUZZER_CHANNEL,
        .timer_sel = WT_BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0};
    ledc_channel_config(&channel);
}

static void buzzer_play_freq(uint32_t freq)
{
    if (freq == 0)
    {
        ledc_stop(LEDC_LOW_SPEED_MODE, WT_BUZZER_CHANNEL, 0);
        return;
    }

    ledc_set_freq(LEDC_LOW_SPEED_MODE, WT_BUZZER_TIMER, freq);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, WT_BUZZER_CHANNEL, 512);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, WT_BUZZER_CHANNEL);
}

static void buzzer_stop()
{
    ledc_stop(LEDC_LOW_SPEED_MODE, WT_BUZZER_CHANNEL, 0);
}

static void play_sound(wt_buzzer_note_t *sound, int len)
{
    for (int i = 0; i < len; i++)
    {
        buzzer_play_freq(sound[i].freq);
        vTaskDelay(pdMS_TO_TICKS(sound[i].duration_ms));
    }
    buzzer_stop();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void wt_sound_play_event(wt_sound_event_t event)
{
    if (wt_buzzer_queue)
    {
        xQueueSend(wt_buzzer_queue, &event, 0);
    }
}

/* ------------------------------------------------------------------ */
/*  Sound Task                                                        */
/* ------------------------------------------------------------------ */

void wt_task_sound(void *pvParameter)
{
    buzzer_init();
    wt_buzzer_queue = xQueueCreate(5, sizeof(wt_sound_event_t));

    wt_sound_event_t event;

    for (;;)
    {
        if (xQueueReceive(wt_buzzer_queue, &event, portMAX_DELAY))
        {
            if (event >= TOTAL_SOUNDS)
                continue;

            wt_buzzer_note_t *sound = wt_sound[event];
            uint8_t len = sizeof(wt_sound[event])/sizeof(wt_buzzer_note_t);

            APPLOG_I("Sound event: %d", event);

            play_sound(sound, len);
        }
    }
}