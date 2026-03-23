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
#define RMT_GPIO_NUM 14            ///< GPIO pin connected to strip data-in
#define FRAME_MS 20                ///< Render period in ms (50 fps)
#define PULSE_SPEED 0.08f          ///< Phase increment/frame, PULSE  (~1.6 s/breath)
#define RAINBOW_SPEED 0.04f        ///< Phase increment/frame, RAINBOW (~3.1 s/cycle)
#define WAVE_HUE_STEP 20           ///< Hue degrees between adjacent segments in WAVE
#define COLON_BLINK_FRAMES 25      ///< Half-period in frames for colon blink (500 ms)
#define INTENSITY_SLEW_STEP 4      ///< Max brightness delta applied per frame

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

// Digit base indices (from your layout)
static const int digit_base[4] = {
    0,  // D1
    14, // D2
    30, // D3 (skip colon 28–29)
    44  // D4
};

// Segment order: G F A B C D E
// Map segment_index (0–6) to physical order
static const int seg_order[7] = {
    0, // G
    1, // F
    2, // A
    3, // B
    4, // C
    5, // D
    6  // E
};

static int get_physical_led_index(int visual_digit, int segment_index, int led_in_seg)
{
    int base = digit_base[visual_digit];

    int seg = seg_order[segment_index];

    return base + seg * WT_SEGD_LEDS_PER_SEG + led_in_seg;
}

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
        // float b = sinf(phase) * 0.5f + 0.5f;
        // color = wt_scale_color_intensity(req->color_on, (uint8_t)(b * req->intensity));
        // break;

        // /* BREATHING WAVE */
        // int global_seg = visual_index * WT_SEGD_SEGS_PER_DIGIT + segment_index;
        // float offset = global_seg * 0.5f;
        // float b = sinf(phase + offset) * 0.5f + 0.5f;
        // color = wt_scale_color_intensity(req->color_on, (uint8_t)(b * req->intensity));
        // break;

        // /* FIRE CRACKER */
        // float noise = (float)(rand() % 100) / 100.0f; // 0–1
        // float b = 0.7f + noise * 0.3f;
        // color = wt_scale_color_intensity(req->color_on, (uint8_t)(b * req->intensity));
        // break;

        // /* SCAN LINE */
        // int total = WT_SEGD_TOTAL_LEDS;
        // float pos = fmodf(phase * 6.0f, total);
        // int global_seg = visual_index * WT_SEGD_SEGS_PER_DIGIT + segment_index;
        // float dist = fabsf(global_seg - pos);
        // float b = expf(-dist * 1.5f); // sharp falloff
        // color = wt_scale_color_intensity(req->color_on, (uint8_t)(b * req->intensity));
        // break;

        // /* DUAL COLOR FLOW */
        // int global_seg = visual_index * WT_SEGD_SEGS_PER_DIGIT + segment_index;
        // float p = phase + global_seg * 0.3f;
        // int hue1 = ((int)(p * 180.0f)) % 360;
        // int hue2 = (hue1 + 180) % 360;
        // float mix = sinf(p) * 0.5f + 0.5f;
        // wt_segd_color_t c1 = wt_hsv_to_rgb(hue1, 255, req->intensity);
        // wt_segd_color_t c2 = wt_hsv_to_rgb(hue2, 255, req->intensity);
        // color.red = (uint8_t)(c1.red * mix + c2.red * (1.0f - mix));
        // color.green = (uint8_t)(c1.green * mix + c2.green * (1.0f - mix));
        // color.blue = (uint8_t)(c1.blue * mix + c2.blue * (1.0f - mix));
        // break;

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
        float flow_speed = 0.5f;
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

    \param[in]  on     True = colon illuminated, false = colon off.
    \param[in]  req    Current display request (color / animation).
    \param[in]  phase  Current animation phase in radians.
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

uint8_t coordsX[WT_SEGD_TOTAL_LEDS] = {
    242, 230, 217, 217, 230, 242, 255, 255, 255, 255, 242, 230, 217, 217, 179, 166, 153, 153, 166, 179,
    191, 191, 191, 191, 179, 166, 153, 153, 128, 128, 89, 77, 64, 64, 77, 89, 102, 102, 102, 102, 89,
    77, 64, 64, 26, 13, 0, 0, 13, 26, 38, 38, 38,
    38, 26, 13, 0, 0};
uint8_t coordsY[WT_SEGD_TOTAL_LEDS] = {
    128, 128, 85, 43, 0, 0, 43, 85, 170, 213,
    255, 255, 213, 170, 128, 128, 85, 43, 0,
    0, 43, 85, 170, 213, 255, 255, 213, 170,
    170, 85, 128, 128, 85, 43, 0, 0, 43, 85,
    170, 213, 255, 255, 213, 170, 128, 128,
    85, 43, 0, 0, 43, 85, 170, 213, 255, 255, 213, 170};
uint8_t angles[WT_SEGD_TOTAL_LEDS] = {
    125, 125, 118, 113, 110, 112, 117, 121, 130, 134, 139, 141, 137, 131, 122, 119, 96, 86, 89,
    96, 107, 114, 132, 141, 153, 159, 159, 141, 223, 51, 6, 4, 11, 17, 27, 32, 32, 22, 247, 233, 230, 234, 244, 251, 2, 2, 6, 9, 14, 16, 13, 8, 252, 247, 243, 245, 249, 253};
uint8_t radii[WT_SEGD_TOTAL_LEDS] = {201, 178, 158, 165, 196, 217, 232, 227, 225, 227, 209, 187, 158, 154, 84, 60, 50, 69, 102, 117, 122, 112, 107, 112, 102, 84, 50, 37, 17, 37, 84, 107, 135, 143, 135, 117, 84, 69, 60, 69, 102, 122, 135, 130, 201, 225, 251, 255, 239, 217, 187, 181, 178, 181, 209, 232, 251, 248};

static int t = 0;
static void effect_spiral_energy(void)
{
    for (int i = 0; i < WT_SEGD_TOTAL_LEDS; i++)
    {
        int hue = angles[i] * 3 + radii[i] * 2 + t;

        int wave = (sin((radii[i] + t) * 0.05) + 1.0) * 127;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, wave);
        c = wt_scale_color_intensity(c, 140);

        wt_set_led_buf(i, c);
    }

    t += 3;
}

static int t_rain = 0;

void effect_rainbow_ring()
{
    for (int i = 0; i < WT_SEGD_TOTAL_LEDS; i++)
    {
        int hue = angles[i] * 2 + t_rain;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, 180);
        wt_set_led_buf(i, c);
    }

    t_rain += 2;
}

static int t_ripple = 0;

void effect_ripple()
{
    for (int i = 0; i < WT_SEGD_TOTAL_LEDS; i++)
    {
        int wave = (sin((radii[i] + t_ripple) * 0.08) + 1.0) * 127;

        wt_segd_color_t c = wt_hsv_to_rgb(160, 255, wave); // blue tones
        wt_set_led_buf(i, c);
    }

    t_ripple += 3;
}

static int t_galaxy = 0;

void effect_galaxy()
{
    for (int i = 0; i < WT_SEGD_TOTAL_LEDS; i++)
    {
        int hue = angles[i] * 4 + t_galaxy;

        int brightness =
            (sin((radii[i] * 0.1 + t_galaxy * 0.05)) + 1.0) * 127;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 200, brightness);
        wt_set_led_buf(i, c);
    }

    t_galaxy += 2;
}

static int t_flow = 0;

void effect_xy_flow()
{
    for (int i = 0; i < WT_SEGD_TOTAL_LEDS; i++)
    {
        int hue =
            coordsX[i] +
            coordsY[i] +
            t_flow;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, 200);
        wt_set_led_buf(i, c);
    }

    t_flow += 1;
}

static int t_wave = 0;

void effect_shockwave()
{
    for (int i = 0; i < WT_SEGD_TOTAL_LEDS; i++)
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

void run_effects(int mode)
{
    switch (mode)
    {
    case 0:
        effect_rainbow_ring();
        break;
    case 1:
        effect_ripple();
        break;
    case 3:
        effect_galaxy();
        break;
    case 5:
        effect_xy_flow();
        break;
    case 6:
        effect_shockwave();
        break;
    }
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
    wt_segd_request_t target = current;

    wt_segd_frame_t frame = {0};
    wt_segd_prepare_frame(&current, &frame);

    float pulse_phase = 0.0f;   ///< Phase accumulator for PULSE animation
    float rainbow_phase = 0.0f; ///< Phase accumulator for RAINBOW / WAVE animation
    uint32_t tick = 0;          ///< Frame counter used for colon blink timing

    APPLOG_I("Render loop started (58 LEDs: D1@0 D2@14 colon@28 D3@30 D4@44)");

    while (1)
    {
        wt_segd_request_t new_req;
        if (xQueueReceive(wt_segd_queue, &new_req, 0) == pdTRUE)
        {
            target = new_req;
            uint8_t keep_intensity = current.intensity;
            current = new_req;
            current.intensity = keep_intensity;
            // APPLOG_I("Request: mode=%d value=%d", current.mode, current.value);
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

        run_effects(5);

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
