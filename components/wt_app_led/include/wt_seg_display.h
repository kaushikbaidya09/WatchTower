/*!
    \file   wt_seg_display.h
    \brief  4-digit 7-segment LED display driver interface.

    Provides segment encoding, character/symbol lookup, display request
    structures, and the shared FreeRTOS queue used to pass display data
    from any application task to the LED render task (wt_task_led).

    Hardware layout (58 WS2812 LEDs total):

    \code
     Visual:      [ D4 ][ D3 ] : [ D2 ][ D1 ]

     Data IN
       │
       ▼
     ┌──────────┬──────────┬─────────┬──────────┬──────────┐
     │ digit 1  │ digit 2  │  colon  │ digit 3  │ digit 4  │
     │ LED 0-13 │ LED14-27 │ LED28-29│ LED30-43 │ LED44-57 │
     │(rightmost│          │         │          │ leftmost)│
     └──────────┴──────────┴─────────┴──────────┴──────────┘
    \endcode

    Strip order per digit: G → F → A → B → C → D → E  (2 LEDs per segment).
 */

#ifndef WT_SEG_DISPLAY_H
#define WT_SEG_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/*!
    Segment bit definitions  (A=bit0 … G=bit6)

    \code
      AAA
     F   B
     F   B
      GGG
     E   C
     E   C
      DDD
    \endcode
 */
#define WT_SEGD_A (1 << 0) /*!< Top horizontal segment          */
#define WT_SEGD_B (1 << 1) /*!< Top-right vertical segment      */
#define WT_SEGD_C (1 << 2) /*!< Bottom-right vertical segment   */
#define WT_SEGD_D (1 << 3) /*!< Bottom horizontal segment       */
#define WT_SEGD_E (1 << 4) /*!< Bottom-left vertical segment    */
#define WT_SEGD_F (1 << 5) /*!< Top-left vertical segment       */
#define WT_SEGD_G (1 << 6) /*!< Middle horizontal segment       */
#define WT_SEGD_NONE 0     /*!< All segments off (blank digit)  */

/*!
    Hardware constants
 */
#define WT_SEGD_LEDS_PER_SEG 2                                                                ///< WS2812 LEDs per segment
#define WT_SEGD_SEGS_PER_DIGIT 7                                                              ///< Segments per digit
#define WT_SEGD_LEDS_PER_DIGIT (WT_SEGD_LEDS_PER_SEG * WT_SEGD_SEGS_PER_DIGIT)                ///< LEDs per digit (14)
#define WT_SEGD_NUM_DIGITS 4                                                                  ///< Total number of digits
#define WT_SEGD_LEDS_COLON 2                                                                  ///< LEDs used for the colon
#define WT_SEGD_COLON_LED_OFFSET 28                                                           ///< First LED index of the colon
#define WT_SEGD_TOTAL_LEDS (WT_SEGD_LEDS_PER_DIGIT * WT_SEGD_NUM_DIGITS + WT_SEGD_LEDS_COLON) ///< Total LEDs in strip (58)

/*!
    Animation mode applied to illuminated segments each render frame.
 */
typedef enum
{
    WT_SEGD_ANIM_SOLID = 0, ///< Static color, no animation
    WT_SEGD_ANIM_PULSE,     ///< Breathing / fade in-out using a sine wave
    WT_SEGD_ANIM_RAINBOW,   ///< All ON segments share one continuously cycling hue
    WT_SEGD_ANIM_WAVE,      ///< Each segment receives a staggered hue offset (wave)
} wt_segd_anim_t;

/*!
    Selects how wt_segd_request_t value / text / raw is interpreted.
 */
typedef enum
{
    WT_SEGD_MODE_NUMBER = 0, ///< Integer 0-9999  shown as  D4 D3 : D2 D1
    WT_SEGD_MODE_TIME,       ///< System wall-clock time shown as HH:MM  (req->value is ignored)
    WT_SEGD_MODE_TEXT,       ///< Up to 4 ASCII characters  (left → right)
    WT_SEGD_MODE_RAW,        ///< Raw segment bitmasks for each digit
} wt_segd_mode_t;

/*!
    24-bit RGB color value.
 */
typedef struct
{
    uint8_t r; ///< Red   channel (0-255)
    uint8_t g; ///< Green channel (0-255)
    uint8_t b; ///< Blue  channel (0-255)
} wt_segd_color_t;

/*! \brief Construct a wt_segd_color_t literal inline. */
#define WT_SEGD_COLOR(r, g, b) ((wt_segd_color_t){(r), (g), (b)})

#define WT_SEGD_RED WT_SEGD_COLOR(255, 0, 0)       /*!< Preset: red     */
#define WT_SEGD_GREEN WT_SEGD_COLOR(0, 255, 0)     /*!< Preset: green   */
#define WT_SEGD_BLUE WT_SEGD_COLOR(0, 0, 255)      /*!< Preset: blue    */
#define WT_SEGD_WHITE WT_SEGD_COLOR(255, 255, 255) /*!< Preset: white   */
#define WT_SEGD_YELLOW WT_SEGD_COLOR(255, 255, 0)  /*!< Preset: yellow  */
#define WT_SEGD_CYAN WT_SEGD_COLOR(0, 255, 255)    /*!< Preset: cyan    */
#define WT_SEGD_MAGENTA WT_SEGD_COLOR(255, 0, 255) /*!< Preset: magenta */
#define WT_SEGD_ORANGE WT_SEGD_COLOR(255, 128, 0)  /*!< Preset: orange  */
#define WT_SEGD_DIM WT_SEGD_COLOR(5, 5, 5)         /*!< Preset: very dim white (segment outline) */
#define WT_SEGD_OFF WT_SEGD_COLOR(0, 0, 0)         /*!< Preset: LED fully off                    */

/*!
    Complete display request posted to wt_segd_queue by any task.

    All text/digit ordering is left-to-right visually:
      - text[0] / raw[0]  →  D4 (leftmost)
      - text[3] / raw[3]  →  D1 (rightmost)

    \note Use xQueueOverwrite() so the renderer always sees the latest value.
 */
typedef struct
{

    wt_segd_mode_t mode; ///< Selects how the value / text / raw fields are used

    int value; ///< WT_SEGD_MODE_NUMBER : integer 0-9999
               ///< WT_SEGD_MODE_TIME   : ignored — system wall-clock is used

    char text[5]; ///< WT_SEGD_MODE_TEXT : 4 chars + '\0'  (D4…D1 left→right)
                  ///<

    uint8_t raw[4]; ///< WT_SEGD_MODE_RAW : segment bitmasks [0]=D4 … [3]=D1
                    ///<

    bool colon;       ///< Steady colon on/off
    bool colon_blink; ///< Blink colon at ~1 Hz (overrides colon when true)

    wt_segd_anim_t anim;       ///< Animation mode applied to ON segments
    wt_segd_color_t color_on;  ///< Color for illuminated segments
    wt_segd_color_t color_off; ///< Color for dark segments (WT_SEGD_OFF to hide completely)
    uint8_t intensity;         ///< Master brightness scale 0-255

} wt_segd_request_t;

/*!
    Segment-level render frame produced by wt_segd_prepare_frame()
    and consumed by the LED render loop in wt_task_led().

    Digit ordering is always left-to-right visually:
      - digit[0] = D4 (leftmost)
      - digit[3] = D1 (rightmost)
 */
typedef struct
{
    uint8_t digit[WT_SEGD_NUM_DIGITS]; ///< Segment bitmask for each digit [0]=D4 … [3]=D1
    bool colon;                        ///< Resolved colon state for this frame
} wt_segd_frame_t;

/*!
    \brief  Convert a wt_segd_request_t into a wt_segd_frame_t.
            Call this whenever a new request is dequeued.

    \param[in]  req    Pointer to the incoming display request.
    \param[out] frame  Filled with per-digit segment masks and colon state.
 */
void wt_segd_prepare_frame(const wt_segd_request_t *req, wt_segd_frame_t *frame);

/*!
    \brief  Return the segment bitmask for a single ASCII character.

    Supports digits 0-9, uppercase A-Z, lowercase a-z, and common symbols
    (-, _, =, ~, [, ], (, ), ', ", ?, !, +, space).
    Returns WT_SEGD_G (dash) for any unsupported character.

    \param[in]  c  ASCII character to look up.
    \return     Segment bitmask (bits WT_SEGD_A … WT_SEGD_G).
 */
uint8_t wt_segd_char(char c);

/*!
    FreeRTOS queue handle shared between all tasks and the LED renderer.

    Defined in wt_app_led.c and created inside wt_task_led() at startup.
    Any task may post a wt_segd_request_t here; the LED render loop
    processes items at 50 fps.

    \note    The queue has a length of 1.  Always use xQueueOverwrite() to
             post — it atomically replaces the current item so the renderer
             always sees the latest request without blocking or losing data.
             Never use xQueueSend() on this queue; it will fail when the
             queue is already full.

    \warning Valid only after wt_task_led() has started and created the queue.
 */
extern QueueHandle_t wt_segd_queue;

#endif /* WT_SEG_DISPLAY_H */