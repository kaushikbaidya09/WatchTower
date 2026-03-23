#ifndef WT_SEG_DISPLAY_H
#define WT_SEG_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define WT_SEGD_A (1 << 0) ///< Top horizontal segment
#define WT_SEGD_B (1 << 1) ///< Top-right vertical segment
#define WT_SEGD_C (1 << 2) ///< Bottom-right vertical segment
#define WT_SEGD_D (1 << 3) ///< Bottom horizontal segment
#define WT_SEGD_E (1 << 4) ///< Bottom-left vertical segment
#define WT_SEGD_F (1 << 5) ///< Top-left vertical segment
#define WT_SEGD_G (1 << 6) ///< Middle horizontal segment
#define WT_SEGD_NONE 0     ///< All segments off

#define WT_SEGD_LEDS_PER_SEG 2                                                                ///< WS2812 LEDs per segment
#define WT_SEGD_SEGS_PER_DIGIT 7                                                              ///< Segments per digit
#define WT_SEGD_LEDS_PER_DIGIT (WT_SEGD_LEDS_PER_SEG * WT_SEGD_SEGS_PER_DIGIT)                ///< LEDs per digit
#define WT_SEGD_NUM_DIGITS 4                                                                  ///< Total number of digits
#define WT_SEGD_LEDS_COLON 2                                                                  ///< LEDs used for the colon
#define WT_SEGD_COLON_LED_OFFSET 28                                                           ///< First LED index of the colon
#define WT_SEGD_TOTAL_LEDS (WT_SEGD_LEDS_PER_DIGIT * WT_SEGD_NUM_DIGITS + WT_SEGD_LEDS_COLON) ///< Total LEDs in strip

typedef enum
{
    WT_SEGD_ANIM_SOLID = 0,
    WT_SEGD_ANIM_PULSE,
    WT_SEGD_ANIM_RAINBOW,
    WT_SEGD_ANIM_WAVE,
    WT_SEGD_ANIM_COLOR_FLOW,
} wt_segd_anim_t;

typedef enum
{
    WT_SEGD_MODE_NUMBER = 0,
    WT_SEGD_MODE_TIME,
    WT_SEGD_MODE_TEXT,
    WT_SEGD_MODE_RAW,
} wt_segd_mode_t;

typedef struct
{
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} wt_segd_color_t;

#define WT_SEGD_COLOR(r, g, b) ((wt_segd_color_t){(r), (g), (b)})
#define WT_SEGD_RED (WT_SEGD_COLOR(255, 0, 0))       ///< red
#define WT_SEGD_GREEN (WT_SEGD_COLOR(0, 255, 0))     ///< green
#define WT_SEGD_BLUE (WT_SEGD_COLOR(0, 0, 255))      ///< blue
#define WT_SEGD_WHITE (WT_SEGD_COLOR(255, 255, 255)) ///< white
#define WT_SEGD_YELLOW (WT_SEGD_COLOR(255, 255, 0))  ///< yellow
#define WT_SEGD_CYAN (WT_SEGD_COLOR(0, 255, 255))    ///< cyan
#define WT_SEGD_MAGENTA (WT_SEGD_COLOR(255, 0, 255)) ///< magenta
#define WT_SEGD_ORANGE (WT_SEGD_COLOR(255, 128, 0))  ///< orange
#define WT_SEGD_DIM (WT_SEGD_COLOR(5, 5, 5))         ///< very dim white
#define WT_SEGD_OFF (WT_SEGD_COLOR(0, 0, 0))         ///< Black

typedef struct
{
    wt_segd_mode_t mode;
    int value;                 ///< valid for WT_SEGD_MODE_NUMBER integer 0-9999
    char text[5];              ///< valid for WT_SEGD_MODE_TEXT 4 chars + '\0'
    uint8_t raw[4];            ///< valid for WT_SEGD_MODE_RAW segment bitmasks
    uint8_t time_format;       ///< valid for WT_SEGD_MODE_TIME, 12 or 24
    bool colon;                ///< Steady colon on/off
    bool colon_blink;          ///< Blink colon at ~1 Hz (overrides colon when true)
    wt_segd_anim_t anim;       ///< Animation mode applied to ON segments
    wt_segd_color_t color_on;  ///< Color for illuminated segments
    wt_segd_color_t color_off; ///< Color for dark segments (WT_SEGD_OFF to hide completely)
    uint8_t intensity;         ///< Master brightness scale 0-255
} wt_segd_request_t;

typedef struct
{
    uint8_t digit[WT_SEGD_NUM_DIGITS];
    bool colon;
} wt_segd_frame_t;

typedef struct
{
    wt_segd_request_t request;
    wt_segd_frame_t frame;
    wt_segd_color_t digit_color[WT_SEGD_NUM_DIGITS][WT_SEGD_SEGS_PER_DIGIT];
    wt_segd_color_t colon_color;
    bool colon_on;
} wt_segd_snapshot_t;

/*!
    \brief  Convert a wt_segd_request_t into a wt_segd_frame_t.
 */
void wt_segd_prepare_frame(const wt_segd_request_t *req, wt_segd_frame_t *frame);

void wt_segd_snapshot_set(const wt_segd_snapshot_t *snapshot);
bool wt_segd_snapshot_get(wt_segd_snapshot_t *snapshot);


extern QueueHandle_t wt_segd_queue;

#endif /* WT_SEG_DISPLAY_H */
