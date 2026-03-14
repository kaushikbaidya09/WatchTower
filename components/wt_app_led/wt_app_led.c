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

#define RMT_RESOLUTION_HZ 10000000 ///< RMT clock 10 MHz → 1 tick = 0.1 µs
#define RMT_GPIO_NUM 48            ///< GPIO pin connected to strip data-in
#define FRAME_MS 20                ///< Render period in ms (50 fps)
#define PULSE_SPEED 0.08f          ///< Phase increment/frame, PULSE  (~1.6 s/breath)
#define RAINBOW_SPEED 0.04f        ///< Phase increment/frame, RAINBOW (~3.1 s/cycle)
#define WAVE_HUE_STEP 20           ///< Hue degrees between adjacent segments in WAVE
#define COLON_BLINK_FRAMES 25      ///< Half-period in frames for colon blink (500 ms)

QueueHandle_t wt_segd_queue = NULL;                                                      ///< Shared queue
static uint8_t s_pixels[WT_SEGD_TOTAL_LEDS * 3];                                         ///< Raw GRB byte buffer for all 58 WS2812 LEDs.
static const uint8_t s_strip_pos_to_bit[WT_SEGD_SEGS_PER_DIGIT] = {6, 5, 0, 1, 2, 3, 4}; ///< Strip position to segment bit mapping.
static const int s_digit_led_start[WT_SEGD_NUM_DIGITS] = {44, 30, 14, 0};                ///< Visual digit index to first LED index in the physical strip.

/* ------------------------------------------------------------------ */
/*  WS2812 RMT timing symbols                                         */
/* ------------------------------------------------------------------ */

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
    \brief RMT encoder callback.
 */
static size_t encoder_callback(const void *data, size_t data_size, size_t symbols_written, size_t symbols_free,
                               rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    if (symbols_free < 8)
    {
        return 0;
    }

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

static wt_segd_color_t wt_hsv_to_rgb(int hue, uint8_t sat, uint8_t val)
{
    wt_segd_color_t color = {0, 0, 0};
    if (sat == 0)
    {
        color.red = color.green = color.blue = val;
        return color;
    }

    hue = ((hue % 360) + 360) % 360;
    int region = hue / 60;
    int remainder = (hue - region * 60) * 255 / 60;

    uint8_t p = (uint16_t)val * (255 - sat) >> 8;
    uint8_t q = (uint16_t)val * (255 - ((uint16_t)sat * remainder >> 8)) >> 8;
    uint8_t t = (uint16_t)val * (255 - ((uint16_t)sat * (255 - remainder) >> 8)) >> 8;

    switch (region)
    {
    case 0:
        color.red = val;
        color.green = t;
        color.blue = p;
        break;
    case 1:
        color.red = q;
        color.green = val;
        color.blue = p;
        break;
    case 2:
        color.red = p;
        color.green = val;
        color.blue = t;
        break;
    case 3:
        color.red = p;
        color.green = q;
        color.blue = val;
        break;
    case 4:
        color.red = t;
        color.green = p;
        color.blue = val;
        break;
    default:
        color.red = val;
        color.green = p;
        color.blue = q;
        break;
    }
    return color;
}

/*!
    \brief  Scale an RGB color by a 0-255 intensity factor.
 */
static wt_segd_color_t wt_scale_color_intensity(wt_segd_color_t color, uint8_t intensity)
{
    color.red = (uint16_t)color.red * intensity >> 8;
    color.green = (uint16_t)color.green * intensity >> 8;
    color.blue = (uint16_t)color.blue * intensity >> 8;
    return color;
}

/*!
    \brief Set one pixel into the GRB pixel buffer.
 */
static void wt_set_led_buf(int index, wt_segd_color_t color)
{
    s_pixels[index * 3 + 0] = color.green;
    s_pixels[index * 3 + 1] = color.red;
    s_pixels[index * 3 + 2] = color.blue;
}

/*!
    \brief  Render one digit into the pixel buffer.
 */
static void render_digit(int visual_index, uint8_t seg_mask, const wt_segd_request_t *req, float phase)
{
    int led_offset = s_digit_led_start[visual_index];

    for (int i = 0; i < WT_SEGD_SEGS_PER_DIGIT; i++)
    {
        bool on = (seg_mask >> s_strip_pos_to_bit[i]) & 0x01;
        wt_segd_color_t color;

        if (on)
        {
            int hue = 0;
            switch (req->anim)
            {
            case WT_SEGD_ANIM_SOLID:
                color = wt_scale_color_intensity(req->color_on, req->intensity);
                break;
            case WT_SEGD_ANIM_PULSE:
                float b = sinf(phase) * 0.5f + 0.5f;
                color = wt_scale_color_intensity(req->color_on, (uint8_t)(b * req->intensity));
                break;
            case WT_SEGD_ANIM_RAINBOW:
                hue = (int)(phase * (360.0f / (2.0f * (float)M_PI))) % 360;
                color = wt_hsv_to_rgb(hue, 255, req->intensity);
                break;
            case WT_SEGD_ANIM_WAVE:
                int global_seg = visual_index * WT_SEGD_SEGS_PER_DIGIT + i;
                hue = ((int)(phase * (360.0f / (2.0f * (float)M_PI))) + global_seg * WAVE_HUE_STEP) % 360;
                color = wt_hsv_to_rgb(hue, 255, req->intensity);
                break;
            default:
                color = wt_scale_color_intensity(req->color_on, req->intensity);
                break;
            }
        }
        else
        {
            color = req->color_off;
        }

        int base = led_offset + i * WT_SEGD_LEDS_PER_SEG;
        wt_set_led_buf(base, color);
        wt_set_led_buf(base + 1, color);
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
            color = wt_scale_color_intensity(req->color_on, (uint8_t)(b * req->intensity));
            break;
        }
        case WT_SEGD_ANIM_RAINBOW:
        case WT_SEGD_ANIM_WAVE:
        {
            int hue = (int)(phase * (360.0f / (2.0f * (float)M_PI))) % 360;
            color = wt_hsv_to_rgb(hue, 255, req->intensity);
            break;
        }
        default:
            color = wt_scale_color_intensity(req->color_on, req->intensity);
            break;
        }
    }
    else
    {
        color = req->color_off;
    }

    wt_set_led_buf(WT_SEGD_COLON_LED_OFFSET, color);
    wt_set_led_buf(WT_SEGD_COLON_LED_OFFSET + 1, color);
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

    rmt_encoder_handle_t rtm_encoder_h = NULL;
    rmt_simple_encoder_config_t enc_cfg = {.callback = encoder_callback};
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&enc_cfg, &rtm_encoder_h));
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
        ESP_ERROR_CHECK(rmt_transmit(led_chan, rtm_encoder_h,
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