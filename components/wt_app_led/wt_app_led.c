/*!
    \file   wt_app_led.c
    \brief  WS2812 LED render task implementation.

    Owns the RMT peripheral, the pixel buffer, the shared wt_segd_queue,
    and all animation logic.  Display content is supplied by other tasks via
    the queue using wt_segd_request_t; the segment-to-LED mapping is
    delegated to wt_seg_display.c.

    Physical strip mapping (data-in end = LED 0):
    \code
     visual : D4   D3  :  D2   D1
     LED    : 44   30  28-29  14   0
    \endcode
 */

#include "wt_app_led.h"
#include "wt_seg_display.h"
#include "wt_app_log.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"

/*!
    Configuration constants
 */
#define RMT_RESOLUTION_HZ 10000000 /*!< RMT clock 10 MHz → 1 tick = 0.1 µs   */
#define RMT_GPIO_NUM 48            /*!< GPIO pin connected to strip data-in   */
#define FRAME_MS 20                /*!< Render period in ms (50 fps)          */
#define PULSE_SPEED 0.08f          /*!< Phase increment/frame, PULSE  (~1.6 s/breath) */
#define RAINBOW_SPEED 0.04f        /*!< Phase increment/frame, RAINBOW (~3.1 s/cycle) */
#define WAVE_HUE_STEP 20           /*!< Hue degrees between adjacent segments in WAVE */
#define COLON_BLINK_FRAMES 25      /*!< Half-period in frames for colon blink (500 ms) */

/*!
    Shared queue  (declared extern in wt_seg_display.h).
    Other tasks post wt_segd_request_t items here.
 */
QueueHandle_t wt_segd_queue = NULL;

/*!
    Raw GRB byte buffer for all 58 WS2812 LEDs.
 */
static uint8_t s_pixels[WT_SEGD_TOTAL_LEDS * 3];

/*!
    WS2812 RMT timing symbols
 */
static const rmt_symbol_word_t s_ws2812_zero = {
    /*!< Logical 0: T0H=0.3 µs, T0L=0.9 µs */
    .level0 = 1,
    .duration0 = (uint32_t)(0.3f * RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
    .duration1 = (uint32_t)(0.9f * RMT_RESOLUTION_HZ / 1000000),
};
static const rmt_symbol_word_t s_ws2812_one = {
    /*!< Logical 1: T1H=0.9 µs, T1L=0.3 µs */
    .level0 = 1,
    .duration0 = (uint32_t)(0.9f * RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
    .duration1 = (uint32_t)(0.3f * RMT_RESOLUTION_HZ / 1000000),
};
static const rmt_symbol_word_t s_ws2812_reset = {
    /*!< Reset pulse: 50 µs low */
    .level0 = 0,
    .duration0 = RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
    .level1 = 0,
    .duration1 = RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
};

/*!
    \brief  RMT simple encoder callback.
            Serialises raw byte data into WS2812 RMT symbols, appending a
            reset symbol after the last byte.

    \param[in]  data            Pointer to the pixel byte buffer.
    \param[in]  data_size       Size of the pixel buffer in bytes.
    \param[in]  symbols_written Symbols already written in this transaction.
    \param[in]  symbols_free    Space remaining in the RMT symbol buffer.
    \param[out] symbols         Destination for encoded RMT symbols.
    \param[out] done            Set to 1 when the reset symbol has been written.
    \param[in]  arg             Unused user argument.
    \return     Number of RMT symbols written this call.
 */
static size_t encoder_callback(const void *data, size_t data_size,
                               size_t symbols_written, size_t symbols_free,
                               rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    if (symbols_free < 8)
        return 0;

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = (const uint8_t *)data;

    if (data_pos < data_size)
    {
        size_t n = 0;
        for (int mask = 0x80; mask; mask >>= 1)
        {
            symbols[n++] = (bytes[data_pos] & mask) ? s_ws2812_one : s_ws2812_zero;
        }
        return n;
    }

    symbols[0] = s_ws2812_reset;
    *done = 1;
    return 1;
}

/*!
    Color helpers  (file-local)
 */

/*!
    \brief  Convert HSV to RGB.

    \param[in]  h  Hue        0-359 degrees.
    \param[in]  s  Saturation 0-255.
    \param[in]  v  Value      0-255.
    \return     Resulting wt_segd_color_t.
 */
static wt_segd_color_t hsv_to_rgb(int h, uint8_t s, uint8_t v)
{
    wt_segd_color_t c = {0, 0, 0};
    if (s == 0)
    {
        c.r = c.g = c.b = v;
        return c;
    }

    h = ((h % 360) + 360) % 360;
    int region = h / 60;
    int remainder = (h - region * 60) * 255 / 60;

    uint8_t p = (uint16_t)v * (255 - s) >> 8;
    uint8_t q = (uint16_t)v * (255 - ((uint16_t)s * remainder >> 8)) >> 8;
    uint8_t t = (uint16_t)v * (255 - ((uint16_t)s * (255 - remainder) >> 8)) >> 8;

    switch (region)
    {
    case 0:
        c.r = v;
        c.g = t;
        c.b = p;
        break;
    case 1:
        c.r = q;
        c.g = v;
        c.b = p;
        break;
    case 2:
        c.r = p;
        c.g = v;
        c.b = t;
        break;
    case 3:
        c.r = p;
        c.g = q;
        c.b = v;
        break;
    case 4:
        c.r = t;
        c.g = p;
        c.b = v;
        break;
    default:
        c.r = v;
        c.g = p;
        c.b = q;
        break;
    }
    return c;
}

/*!
    \brief  Scale an RGB color by a 0-255 intensity factor.

    \param[in]  c          Input color.
    \param[in]  intensity  Scale factor (0 = off, 255 = full brightness).
    \return     Scaled color.
 */
static wt_segd_color_t color_scale(wt_segd_color_t c, uint8_t intensity)
{
    c.r = (uint16_t)c.r * intensity >> 8;
    c.g = (uint16_t)c.g * intensity >> 8;
    c.b = (uint16_t)c.b * intensity >> 8;
    return c;
}

/*!
    \brief  Write one pixel into the GRB pixel buffer.

    \param[in]  index  LED index (0-based).
    \param[in]  c      Color to write.
 */
static void set_led(int index, wt_segd_color_t c)
{
    s_pixels[index * 3 + 0] = c.g;
    s_pixels[index * 3 + 1] = c.r;
    s_pixels[index * 3 + 2] = c.b;
}

/*!
    Strip position to segment bit mapping.

    Physical strip order per digit: G  F  A  B  C  D  E  (positions 0-6)
    Bit in mask (A=bit0 … G=bit6):  6  5  0  1  2  3  4
 */
static const uint8_t s_strip_pos_to_bit[WT_SEGD_SEGS_PER_DIGIT] = {6, 5, 0, 1, 2, 3, 4};
//                                                                    G  F  A  B  C  D  E

/*!
    Visual digit index to first LED index in the physical strip.

    \code
     visual index :  0     1     2     3
     digit        :  D4    D3    D2    D1
     LED offset   :  44    30    14     0
    \endcode
 */
static const int s_digit_led_start[WT_SEGD_NUM_DIGITS] = {44, 30, 14, 0};

/*!
    \brief  Render one digit into the pixel buffer.

    Applies the active animation and color to each segment LED pair of the
    specified visual digit position.

    \param[in]  visual_index  0 = D4 (leftmost) … 3 = D1 (rightmost).
    \param[in]  seg_mask      Segment bitmask (WT_SEGD_A … WT_SEGD_G).
    \param[in]  req           Current display request (color / animation).
    \param[in]  phase         Current animation phase in radians (0 … 2π).
 */
static void render_digit(int visual_index, uint8_t seg_mask,
                         const wt_segd_request_t *req, float phase)
{
    int led_offset = s_digit_led_start[visual_index];

    for (int i = 0; i < WT_SEGD_SEGS_PER_DIGIT; i++)
    {

        bool on = (seg_mask >> s_strip_pos_to_bit[i]) & 0x01;
        wt_segd_color_t color;

        if (on)
        {
            switch (req->anim)
            {

            case WT_SEGD_ANIM_SOLID:
                color = color_scale(req->color_on, req->intensity);
                break;

            case WT_SEGD_ANIM_PULSE:
            {
                float b = sinf(phase) * 0.5f + 0.5f;
                color = color_scale(req->color_on, (uint8_t)(b * req->intensity));
                break;
            }

            case WT_SEGD_ANIM_RAINBOW:
            {
                int hue = (int)(phase * (360.0f / (2.0f * (float)M_PI))) % 360;
                color = hsv_to_rgb(hue, 255, req->intensity);
                break;
            }

            case WT_SEGD_ANIM_WAVE:
            {
                /* Each segment receives a hue offset proportional to its
                   global position across the full strip, producing a
                   left-to-right colour wave. */
                int global_seg = visual_index * WT_SEGD_SEGS_PER_DIGIT + i;
                int hue = ((int)(phase * (360.0f / (2.0f * (float)M_PI))) + global_seg * WAVE_HUE_STEP) % 360;
                color = hsv_to_rgb(hue, 255, req->intensity);
                break;
            }

            default:
                color = color_scale(req->color_on, req->intensity);
                break;
            }
        }
        else
        {
            color = req->color_off;
        }

        int base = led_offset + i * WT_SEGD_LEDS_PER_SEG;
        set_led(base, color);
        set_led(base + 1, color);
    }
}

/*!
    \brief  Render the colon LEDs (LED 28-29) into the pixel buffer.

    \param[in]  on     True = colon illuminated, false = colon off.
    \param[in]  req    Current display request (color / animation).
    \param[in]  phase  Current animation phase in radians.
 */
static void render_colon(bool on, const wt_segd_request_t *req, float phase)
{
    wt_segd_color_t color;

    if (on)
    {
        switch (req->anim)
        {
        case WT_SEGD_ANIM_PULSE:
        {
            float b = sinf(phase) * 0.5f + 0.5f;
            color = color_scale(req->color_on, (uint8_t)(b * req->intensity));
            break;
        }
        case WT_SEGD_ANIM_RAINBOW:
        case WT_SEGD_ANIM_WAVE:
        {
            int hue = (int)(phase * (360.0f / (2.0f * (float)M_PI))) % 360;
            color = hsv_to_rgb(hue, 255, req->intensity);
            break;
        }
        default:
            color = color_scale(req->color_on, req->intensity);
            break;
        }
    }
    else
    {
        color = req->color_off;
    }

    set_led(WT_SEGD_COLON_LED_OFFSET, color);
    set_led(WT_SEGD_COLON_LED_OFFSET + 1, color);
}

/*!
    \brief  Main WS2812 LED render task.  See wt_app_led.h for full details.

    \param[in]  pvParameter  Unused; pass NULL when creating the task.
 */
void wt_task_led(void *pvParameter)
{
    // APPLOG_I("---------- LED TASK STARTED ----------");

    /* Create shared display queue */
    wt_segd_queue = xQueueCreate(1, sizeof(wt_segd_request_t));
    if (!wt_segd_queue)
    {
        APPLOG_E("Failed to create wt_segd_queue");
        vTaskDelete(NULL);
        return;
    }

    /* RMT TX channel */
    rmt_channel_handle_t led_chan = NULL;
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = RMT_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &led_chan));

    rmt_encoder_handle_t encoder = NULL;
    rmt_simple_encoder_config_t enc_cfg = {.callback = encoder_callback};
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&enc_cfg, &encoder));
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    rmt_transmit_config_t tx_config = {.loop_count = 0};

    /* Default display state */
    wt_segd_request_t current = {
        .mode = WT_SEGD_MODE_NUMBER,
        .value = 0,
        .colon = true,
        .colon_blink = false,
        .anim = WT_SEGD_ANIM_SOLID,
        .color_on = WT_SEGD_GREEN,
        .color_off = WT_SEGD_DIM,
        .intensity = 200,
    };

    wt_segd_frame_t frame = {0};
    wt_segd_prepare_frame(&current, &frame);

    float pulse_phase = 0.0f;   ///< Phase accumulator for PULSE animation
    float rainbow_phase = 0.0f; ///< Phase accumulator for RAINBOW / WAVE animation
    uint32_t tick = 0;          ///< Frame counter used for colon blink timing

    APPLOG_I("Render loop started (58 LEDs: D1@0 D2@14 colon@28 D3@30 D4@44)");

    while (1)
    {

        /* Dequeue latest request (non-blocking) */
        wt_segd_request_t new_req;
        if (xQueueReceive(wt_segd_queue, &new_req, 0) == pdTRUE)
        {
            current = new_req;
            wt_segd_prepare_frame(&current, &frame);
            // APPLOG_I("Request: mode=%d value=%d", current.mode, current.value);
        }

        /* Resolve colon state for this frame */
        bool colon_on = current.colon_blink
                            ? ((tick / COLON_BLINK_FRAMES) % 2 == 0)
                            : frame.colon;

        /* Select phase for the active animation */
        float phase = (current.anim == WT_SEGD_ANIM_PULSE)
                          ? pulse_phase
                          : rainbow_phase;

        /* Render all 4 digits and the colon */
        for (int v = 0; v < WT_SEGD_NUM_DIGITS; v++)
        {
            render_digit(v, frame.digit[v], &current, phase);
        }
        render_colon(colon_on, &current, phase);

        /* Transmit pixel buffer over RMT */
        ESP_ERROR_CHECK(rmt_transmit(led_chan, encoder,
                                     s_pixels, sizeof(s_pixels), &tx_config));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_chan, portMAX_DELAY));

        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));

        /* Advance animation phases */
        pulse_phase += PULSE_SPEED;
        rainbow_phase += RAINBOW_SPEED;
        if (pulse_phase > 2.0f * (float)M_PI)
        {
            pulse_phase -= 2.0f * (float)M_PI;
        }
        if (rainbow_phase > 2.0f * (float)M_PI)
        {
            rainbow_phase -= 2.0f * (float)M_PI;
        }
        tick++;
    }
}

/*!
    Usage examples for other tasks

    Seconds counter (MM:SS):
    \code
     void counter_task(void *pv) {
         int secs = 0;
         while (1) {
             wt_segd_request_t req = {
                 .mode        = WT_SEGD_MODE_TIME,
                 .value       = secs,
                 .colon       = true,
                 .colon_blink = true,
                 .anim        = WT_SEGD_ANIM_PULSE,
                 .color_on    = WT_SEGD_CYAN,
                 .color_off   = WT_SEGD_OFF,
                 .intensity   = 220,
             };
             if (wt_segd_queue) xQueueOverwrite(wt_segd_queue, &req);
             secs = (secs + 1) % 6000;
             vTaskDelay(pdMS_TO_TICKS(1000));
         }
     }
    \endcode

    4-digit number:
    \code
     wt_segd_request_t req = {
         .mode      = WT_SEGD_MODE_NUMBER,
         .value     = 1234,
         .colon     = true,
         .anim      = WT_SEGD_ANIM_RAINBOW,
         .color_on  = WT_SEGD_WHITE,
         .color_off = WT_SEGD_OFF,
         .intensity = 255,
     };
     xQueueOverwrite(wt_segd_queue, &req);
    \endcode

    4-character text (D4=H  D3=E  D2=L  D1=o):
    \code
     wt_segd_request_t req = {
         .mode      = WT_SEGD_MODE_TEXT,
         .text      = "HELo",
         .colon     = false,
         .anim      = WT_SEGD_ANIM_WAVE,
         .color_on  = WT_SEGD_ORANGE,
         .color_off = WT_SEGD_OFF,
         .intensity = 200,
     };
     xQueueOverwrite(wt_segd_queue, &req);
    \endcode

    Raw segment control:
    \code
     wt_segd_request_t req = {
         .mode = WT_SEGD_MODE_RAW,
         .raw  = {
             WT_SEGD_A | WT_SEGD_G | WT_SEGD_D,   // D4 custom
             WT_SEGD_B | WT_SEGD_C,               // D3 custom
             WT_SEGD_NONE,                         // D2 blank
             WT_SEGD_A | WT_SEGD_B | WT_SEGD_F,   // D1 custom
         },
     };
     xQueueOverwrite(wt_segd_queue, &req);
    \endcode
 */