/*!
    \file   wt_app_led.c
    \brief  WS2812 7-segment display render task (RMT driver + effects).

    \details
    Renders the digit/colon frame plus an always-on background overlay
    effect (see run_effects()) into a single GRB pixel buffer each frame,
    then pushes it out over RMT.
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
#include "esp_err.h"
#include "driver/rmt_tx.h"

#define RMT_RESOLUTION_HZ 10000000 ///< RMT clock 10 MHz → 1 tick = 0.1 µs
#define RMT_GPIO_NUM 14            ///< GPIO pin connected to strip data-in
#define FRAME_MS 20                ///< Render period in ms (50 fps)
#define RMT_WAIT_TIMEOUT_MS 100    ///< Bounded RMT tx-done wait; skip frame instead of rebooting on timeout
#define PULSE_SPEED 0.08f          ///< Phase increment/frame, PULSE  (~1.6 s/breath)
#define RAINBOW_SPEED 0.04f        ///< Phase increment/frame, RAINBOW (~3.1 s/cycle)
#define WAVE_HUE_STEP 20           ///< Hue degrees between adjacent segments in WAVE
#define COLON_BLINK_FRAMES 25      ///< Half-period in frames for colon blink (500 ms)
#define INTENSITY_SLEW_STEP 4      ///< Max brightness delta applied per frame

QueueHandle_t wt_segd_queue = NULL;                                                      ///< Shared queue
static uint8_t s_pixels[WT_SEGD_MAX_TOTAL_LEDS * 3];                                     ///< Raw GRB byte buffer, sized to the max supported LED count.
static const uint8_t s_strip_pos_to_bit[WT_SEGD_SEGS_PER_DIGIT] = {6, 5, 0, 1, 2, 3, 4}; ///< Strip position to segment bit mapping.
static const int s_digit_led_start[WT_SEGD_NUM_DIGITS] = {44, 30, 14, 0};                ///< Visual digit index to first LED index in the physical strip.

static const rmt_symbol_word_t s_ws2812_zero = {
    ///< Logical 0: T0H=0.3 µs, T0L=0.9 µs
    .level0 = 1,
    .duration0 = (uint32_t)(0.3f * RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
    .duration1 = (uint32_t)(0.9f * RMT_RESOLUTION_HZ / 1000000),
};
static const rmt_symbol_word_t s_ws2812_one = {
    ///< Logical 1: T1H=0.9 µs, T1L=0.3 µs
    .level0 = 1,
    .duration0 = (uint32_t)(0.9f * RMT_RESOLUTION_HZ / 1000000),
    .level1 = 0,
    .duration1 = (uint32_t)(0.3f * RMT_RESOLUTION_HZ / 1000000),
};
static const rmt_symbol_word_t s_ws2812_reset = {
    ///< Reset pulse: 50 µs low
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

/*!
    \brief  Convert an HSV color to RGB.
 */
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
    \brief  Physical strip LED index for a digit/strip-position/sub-LED.

    Uses s_digit_led_start as the single source of truth for digit-to-LED
    mapping so this stays consistent with render_digit().
 */
static int get_physical_led_index(int visual_digit, int strip_pos, int led_in_seg)
{
    return s_digit_led_start[visual_digit] + strip_pos * WT_SEGD_LEDS_PER_SEG + led_in_seg;
}

/*!
    \brief  Resolve the color for one segment given its on/off state and the
            currently active animation.
 */
static wt_segd_color_t wt_segment_color_for(int visual_index, int segment_index, bool on,
                                            const wt_segd_request_t *req, float phase)
{
    wt_segd_color_t color;

    if (!on)
    {
        return req->color_off;
    }

    switch (req->anim)
    {
    case WT_SEGD_ANIM_PULSE:
    {
        /* COMET PHYSICAL */
        int led0 = get_physical_led_index(visual_index, segment_index, 0);
        int led1 = get_physical_led_index(visual_index, segment_index, 1);
        float pos = (led0 + led1) * 0.5f;
        float speed = 12.0f;
        float direction = +1.0f;
        float head = fmodf(phase * speed * direction, WT_SEGD_TOTAL_LEDS);
        float dist = fabsf(pos - head);
        if (dist > WT_SEGD_TOTAL_LEDS / 2)
            dist = WT_SEGD_TOTAL_LEDS - dist;
        float b = expf(-dist * 0.25f);
        color = wt_scale_color_intensity(req->color_on, (uint8_t)(b * req->intensity));
        break;
    }
    case WT_SEGD_ANIM_RAINBOW:
    {
        int hue = (int)(phase * (360.0f / (2.0f * (float)M_PI))) % 360;
        color = wt_hsv_to_rgb(hue, 255, req->intensity);
        break;
    }
    case WT_SEGD_ANIM_WAVE:
    {
        int global_seg = visual_index * WT_SEGD_SEGS_PER_DIGIT + segment_index;
        int hue = ((int)(phase * (360.0f / (2.0f * (float)M_PI))) + global_seg * WAVE_HUE_STEP) % 360;
        color = wt_hsv_to_rgb(hue, 255, req->intensity);
        break;
    }
    case WT_SEGD_ANIM_COLOR_FLOW:
    {
        int global_seg = visual_index * WT_SEGD_SEGS_PER_DIGIT + segment_index;
        float offset = global_seg * 0.4f;
        float p = phase + offset;
        int hue = (int)(p * (360.0f / (2.0f * (float)M_PI))) % 360;

        if (hue < 0)
            hue += 360;

        color = wt_hsv_to_rgb(hue, 255, req->intensity);
        break;
    }
    case WT_SEGD_ANIM_SOLID:
    default:
        color = wt_scale_color_intensity(req->color_on, req->intensity);
        break;
    }

    return color;
}

/*!
    \brief  Resolve the color for the colon LEDs given their on/off state and
            the currently active animation.
 */
static wt_segd_color_t wt_colon_color_for(bool on, const wt_segd_request_t *req, float phase)
{
    return wt_segment_color_for(0, 0, on, req, phase);
}

/*!
    \brief  Render one digit into the pixel buffer.
 */
static void render_digit(int visual_index, uint8_t seg_mask, const wt_segd_request_t *req,
                         float phase, wt_segd_snapshot_t *snapshot)
{
    int led_offset = s_digit_led_start[visual_index];

    for (int i = 0; i < WT_SEGD_SEGS_PER_DIGIT; i++)
    {
        bool on = (seg_mask >> s_strip_pos_to_bit[i]) & 0x01;
        wt_segd_color_t color = wt_segment_color_for(visual_index, i, on, req, phase);
        if (snapshot)
        {
            snapshot->digit_color[visual_index][s_strip_pos_to_bit[i]] = color;
        }

        int base = led_offset + i * WT_SEGD_LEDS_PER_SEG;
        wt_set_led_buf(base, color);
        wt_set_led_buf(base + 1, color);
    }
}

/*!
    \brief  Render the colon LEDs (LED 28-29) into the pixel buffer.
 */
static void render_colon(bool on, const wt_segd_request_t *req, float phase, wt_segd_snapshot_t *snapshot)
{
    wt_segd_color_t color = wt_colon_color_for(on, req, phase);
    if (snapshot)
    {
        snapshot->colon_color = color;
    }

    wt_set_led_buf(WT_SEGD_COLON_LED_OFFSET, color);
    wt_set_led_buf(WT_SEGD_COLON_LED_OFFSET + 1, color);
}

/* Hand-calibrated physical position/angle data for the fixed 58-LED digit
   board, used only by the spatial background effects below (never by digit
   rendering). Any LEDs beyond WT_SEGD_TOTAL_LEDS (an optional extra strip a
   builder wires up, up to WT_SEGD_MAX_TOTAL_LEDS) have no hand-calibrated
   data, so build_led_layout() synthesizes a plausible ring layout for them. */
static const uint8_t k_coordsX_base[WT_SEGD_TOTAL_LEDS] = {
    242, 230, 217, 217, 230, 242, 255, 255, 255, 255, 242, 230, 217, 217, 179, 166, 153, 153, 166, 179,
    191, 191, 191, 191, 179, 166, 153, 153, 128, 128, 89, 77, 64, 64, 77, 89, 102, 102, 102, 102, 89,
    77, 64, 64, 26, 13, 0, 0, 13, 26, 38, 38, 38,
    38, 26, 13, 0, 0};
static const uint8_t k_coordsY_base[WT_SEGD_TOTAL_LEDS] = {
    128, 128, 85, 43, 0, 0, 43, 85, 170, 213,
    255, 255, 213, 170, 128, 128, 85, 43, 0,
    0, 43, 85, 170, 213, 255, 255, 213, 170,
    170, 85, 128, 128, 85, 43, 0, 0, 43, 85,
    170, 213, 255, 255, 213, 170, 128, 128,
    85, 43, 0, 0, 43, 85, 170, 213, 255, 255, 213, 170};
static const uint8_t k_angles_base[WT_SEGD_TOTAL_LEDS] = {
    125, 125, 118, 113, 110, 112, 117, 121, 130, 134, 139, 141, 137, 131, 122, 119, 96, 86, 89,
    96, 107, 114, 132, 141, 153, 159, 159, 141, 223, 51, 6, 4, 11, 17, 27, 32, 32, 22, 247, 233, 230, 234, 244, 251, 2, 2, 6, 9, 14, 16, 13, 8, 252, 247, 243, 245, 249, 253};
static const uint8_t k_radii_base[WT_SEGD_TOTAL_LEDS] = {201, 178, 158, 165, 196, 217, 232, 227, 225, 227, 209, 187, 158, 154, 84, 60, 50, 69, 102, 117, 122, 112, 107, 112, 102, 84, 50, 37, 17, 37, 84, 107, 135, 143, 135, 117, 84, 69, 60, 69, 102, 122, 135, 130, 201, 225, 251, 255, 239, 217, 187, 181, 178, 181, 209, 232, 251, 248};

/* Runtime layout tables actually read by the effects: indices
   [0, WT_SEGD_TOTAL_LEDS) are copied verbatim from the k_*_base tables above;
   indices [WT_SEGD_TOTAL_LEDS, led_count) are synthesized by
   build_led_layout() for whatever extra LEDs the current led_count adds. */
static uint8_t coordsX[WT_SEGD_MAX_TOTAL_LEDS];
static uint8_t coordsY[WT_SEGD_MAX_TOTAL_LEDS];
static uint8_t angles[WT_SEGD_MAX_TOTAL_LEDS];
static uint8_t radii[WT_SEGD_MAX_TOTAL_LEDS];

/*!
    \brief  (Re)builds the runtime layout tables for the currently configured
            led_count: the fixed 58-LED digit board keeps its hand-calibrated
            data, and any extra LEDs beyond that are placed evenly around a
            synthetic outer ring so the spatial effects have something
            reasonable to animate across.
 */
static void build_led_layout(int led_count)
{
    if (led_count > WT_SEGD_MAX_TOTAL_LEDS)
    {
        led_count = WT_SEGD_MAX_TOTAL_LEDS;
    }

    int base_count = (led_count < WT_SEGD_TOTAL_LEDS) ? led_count : WT_SEGD_TOTAL_LEDS;
    memcpy(coordsX, k_coordsX_base, (size_t)base_count);
    memcpy(coordsY, k_coordsY_base, (size_t)base_count);
    memcpy(angles, k_angles_base, (size_t)base_count);
    memcpy(radii, k_radii_base, (size_t)base_count);

    int extra_count = led_count - WT_SEGD_TOTAL_LEDS;
    for (int i = 0; i < extra_count; i++)
    {
        float frac = (float)i / (float)extra_count;
        float theta = frac * 2.0f * (float)M_PI;
        uint8_t radius = 220;

        angles[WT_SEGD_TOTAL_LEDS + i] = (uint8_t)(frac * 255.0f);
        radii[WT_SEGD_TOTAL_LEDS + i] = radius;
        coordsX[WT_SEGD_TOTAL_LEDS + i] = (uint8_t)(128.0f + 100.0f * cosf(theta));
        coordsY[WT_SEGD_TOTAL_LEDS + i] = (uint8_t)(128.0f + 100.0f * sinf(theta));
    }
}

static int t_rain = 0;

/*!
    \brief  Rainbow-ring background overlay effect.
 */
static void effect_rainbow_ring(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int hue = angles[i] * 2 + t_rain;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, 180);
        wt_set_led_buf(i, c);
    }

    /* hue is mod-360'd inside wt_hsv_to_rgb, so wrapping the phase itself at
       360 is an exact no-op visually while keeping the counter bounded
       (unbounded "static int" growth is a CERT INT30-C signed-overflow risk
       on a device that stays up for months). */
    t_rain = (t_rain + 2) % 360;
}

static int t_ripple = 0;

/*!
    \brief  Ripple background overlay effect.
 */
static void effect_ripple(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int wave = (sin((radii[i] + t_ripple) * 0.08) + 1.0) * 127;

        wt_segd_color_t c = wt_hsv_to_rgb(160, 255, wave); // blue tones
        wt_set_led_buf(i, c);
    }

    /* t_ripple feeds a sin() argument with a non-integer period, so it can't
       wrap seamlessly at a small bound reset it at a large bound instead,
       just to keep it defined (see t_rain's comment for why this matters). */
    t_ripple += 3;
    if (t_ripple >= 1000000000)
        t_ripple = 0;
}

static int t_galaxy = 0;

/*!
    \brief  Galaxy background overlay effect.
 */
static void effect_galaxy(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int hue = angles[i] * 4 + t_galaxy;

        int brightness =
            (sin((radii[i] * 0.1 + t_galaxy * 0.05)) + 1.0) * 127;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 200, brightness);
        wt_set_led_buf(i, c);
    }

    /* Same rationale as effect_ripple()'s t_ripple wrap. */
    t_galaxy += 2;
    if (t_galaxy >= 1000000000)
        t_galaxy = 0;
}

static int t_flow = 0;

/*!
    \brief  XY-flow background overlay effect.
 */
static void effect_xy_flow(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int hue =
            coordsX[i] +
            coordsY[i] +
            t_flow;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, 200);
        wt_set_led_buf(i, c);
    }

    /* Same rationale as effect_rainbow_ring()'s t_rain wrap: hue is the only
       consumer and it's mod-360'd internally, so wrapping here at 360 is exact. */
    t_flow = (t_flow + 1) % 360;
}

static int t_wave = 0;

/*!
    \brief  Shockwave background overlay effect.
 */
static void effect_shockwave(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int dist = abs(radii[i] - t_wave);

        uint8_t bright = (dist < 20) ? (255 - dist * 10) : 0;

        wt_segd_color_t c = wt_hsv_to_rgb(0, 255, bright);
        wt_set_led_buf(i, c);
    }

    t_wave += 5;
    if (t_wave > 255)
        t_wave = 0;
}

/*!
    \brief  No-op background effect leaves the digit/colon render from this
            frame untouched. This is the default pattern (see
            wt_led_anim_effect_from_name) so that run_effects() can run
            unconditionally every frame without a background overlay
            appearing until one is explicitly selected.
 */
static void effect_solid(void)
{
}

/*!
    \brief  Render the selected background overlay pattern on top of the
            digit/colon render, overwriting whichever LEDs the pattern
            touches. Runs every frame regardless of display mode; mode 2
            (solid) is a no-op so it never touches the buffer.
    \param[in]  mode       Effect pattern index (see wt_led_anim_effect_from_name()).
    \param[in]  led_count  Number of LEDs to animate (see wt_segd_request_t.led_count).
 */
static void run_effects(int mode, int led_count)
{
    switch (mode)
    {
    case 0:
        effect_rainbow_ring(led_count);
        break;
    case 1:
        effect_ripple(led_count);
        break;
    case 2:
        effect_solid();
        break;
    case 3:
        effect_galaxy(led_count);
        break;
    case 5:
        effect_xy_flow(led_count);
        break;
    case 6:
        effect_shockwave(led_count);
        break;
    default:
        break;
    }
}

/*!
    \brief  Map an anim-effect name (e.g. "ripple") to the run_effects() mode
            integer used in wt_segd_request_t.anim_effect. Unknown/NULL
            names return the solid (no animation) mode (2).
    \param[in]  name  Anim effect name; NULL is treated as unknown.
    \return Mode integer for run_effects().
 */
uint8_t wt_led_anim_effect_from_name(const char *name)
{
    static const struct
    {
        const char *name;
        uint8_t mode;
    } k_anim_effects[] = {
        {"rainbow_ring", 0},
        {"ripple", 1},
        {"solid", 2},
        {"galaxy", 3},
        {"xy_flow", 5},
        {"shockwave", 6},
    };

    if (name)
    {
        for (size_t i = 0; i < sizeof(k_anim_effects) / sizeof(k_anim_effects[0]); i++)
        {
            if (strcmp(name, k_anim_effects[i].name) == 0)
            {
                return k_anim_effects[i].mode;
            }
        }
    }
    return 2;
}

/*!
    \brief  Main WS2812 LED render task.  See wt_app_led.h for full details.

    \param[in]  pvParameter  Unused; pass NULL when creating the task.
 */
void wt_task_led(void *pvParameter)
{
    // wt_log_info("---------- LED TASK STARTED ----------");

    if (!wt_segd_queue)
    {
        wt_log_error("wt_segd_queue not created before wt_task_led started");
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
        .anim = WT_SEGD_ANIM_COLOR_FLOW,
        .color_on = WT_SEGD_GREEN,
        .color_off = WT_SEGD_DIM,
        .intensity = 255,
        .led_count = WT_SEGD_TOTAL_LEDS,
    };
    wt_segd_request_t target = current;

    wt_segd_frame_t frame = {0};
    wt_segd_prepare_frame(&current, &frame);

    int applied_led_count = current.led_count;
    build_led_layout(applied_led_count);

    float pulse_phase = 0.0f;   ///< Phase accumulator for PULSE animation
    float rainbow_phase = 0.0f; ///< Phase accumulator for RAINBOW / WAVE animation
    uint32_t tick = 0;          ///< Frame counter used for colon blink timing

    wt_log_info("Render loop started (%d LEDs: D1@0 D2@14 colon@28 D3@30 D4@44)", applied_led_count);

    TickType_t last_wake = xTaskGetTickCount();

    while (1)
    {
        wt_segd_request_t new_req;
        if (xQueueReceive(wt_segd_queue, &new_req, 0) == pdTRUE)
        {
            target = new_req;
            uint8_t keep_intensity = current.intensity;
            current = new_req;
            current.intensity = keep_intensity;
            // wt_log_info("Request: mode=%d value=%d", current.mode, current.value);

            if (current.led_count != applied_led_count)
            {
                build_led_layout(current.led_count);
                applied_led_count = current.led_count;
            }
        }

        if (current.intensity < target.intensity)
        {
            int next = current.intensity + INTENSITY_SLEW_STEP;
            current.intensity = (next > target.intensity) ? target.intensity : (uint8_t)next;
        }
        else if (current.intensity > target.intensity)
        {
            int next = current.intensity - INTENSITY_SLEW_STEP;
            current.intensity = (next < target.intensity) ? target.intensity : (uint8_t)next;
        }

        wt_segd_prepare_frame(&current, &frame);

        /* Resolve colon state for this frame */
        bool colon_on = current.colon_blink
                            ? ((tick / COLON_BLINK_FRAMES) % 2 == 0)
                            : frame.colon;

        /* Select phase for the active animation */
        float phase = (current.anim == WT_SEGD_ANIM_PULSE)
                          ? pulse_phase
                          : rainbow_phase;

        wt_segd_snapshot_t snapshot = {
            .request = current,
            .frame = frame,
            .colon_on = colon_on,
        };

        /* Render all 4 digits and the colon */
        for (int v = 0; v < WT_SEGD_NUM_DIGITS; v++)
        {
            render_digit(v, frame.digit[v], &current, phase, &snapshot);
        }
        render_colon(colon_on, &current, phase, &snapshot);
        wt_segd_snapshot_set(&snapshot);

        /* Background overlay pattern, runs every frame. Mode 2 (solid) is a
           no-op, so the digit/colon render above stays untouched until a
           real pattern is selected. */
        run_effects(current.anim_effect, applied_led_count);

        /* Transmit pixel buffer over RMT.  A transient RMT error skips this
           frame and logs rather than rebooting the device. */
        esp_err_t tx_err = rmt_transmit(led_chan, rtm_encoder_h,
                                        s_pixels, (size_t)applied_led_count * 3, &tx_config);
        if (tx_err != ESP_OK)
        {
            wt_log_warn("rmt_transmit failed: %s skipping frame", esp_err_to_name(tx_err));
        }
        else
        {
            esp_err_t wait_err = rmt_tx_wait_all_done(led_chan, pdMS_TO_TICKS(RMT_WAIT_TIMEOUT_MS));
            if (wait_err != ESP_OK)
            {
                wt_log_warn("rmt_tx_wait_all_done failed: %s skipping frame", esp_err_to_name(wait_err));
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FRAME_MS));

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
