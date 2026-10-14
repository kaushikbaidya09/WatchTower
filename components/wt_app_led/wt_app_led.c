//=============================================================================
/*!
    \file    wt_app_led.c
    \brief   LED display task: renders the digits and background effects.
*/
//=============================================================================

// Each frame renders the digits and colon, then the background effect,
// into one GRB pixel buffer and sends it over RMT.

#include "wt_app_led.h"
#include "esp_attr.h"
#include "wt_seg_display.h"
#include "wt_app_log.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/rmt_tx.h"

#define RMT_RESOLUTION_HZ     10000000 ///< RMT clock 10 MHz → 1 tick = 0.1 µs
#define RMT_GPIO_NUM          14       ///< GPIO pin connected to strip data-in
#define FRAME_MS              20       ///< Render period in ms (50 fps)
#define RMT_WAIT_TIMEOUT_MS   100      ///< Longest wait for a frame to finish sending
#define COLON_BLINK_FRAMES    25       ///< Half-period in frames for colon blink (500 ms)
#define INTENSITY_SLEW_STEP   4        ///< Max brightness delta applied per frame
#define RMT_WARN_EVERY_FRAMES 250 ///< Log a persisting RMT fault once per this many frames (5 s)

QueueHandle_t wt_segd_queue = NULL; ///< Display requests for the LED task
static uint8_t s_pixels[WT_SEGD_MAX_TOTAL_LEDS *
                        3]; ///< Pixel data in GRB order, room for the most LEDs supported
static const uint8_t s_strip_pos_to_bit[WT_SEGD_SEGS_PER_DIGIT] = {
    6, 5, 0, 1, 2, 3, 4}; ///< Segment bit for each LED pair along a digit
static const int s_digit_led_start[WT_SEGD_NUM_DIGITS] = {
    44, 30, 14, 0}; ///< First LED of each digit, left to right

// The encoder runs in the RMT interrupt, which keeps running while flash is
// written (settings saves), so it and its data must be in RAM.
static DRAM_ATTR const rmt_symbol_word_t s_ws2812_zero = {
    ///< Logical 0: T0H=0.3 µs, T0L=0.9 µs
    .level0    = 1,
    .duration0 = (uint32_t)(0.3f * RMT_RESOLUTION_HZ / 1000000),
    .level1    = 0,
    .duration1 = (uint32_t)(0.9f * RMT_RESOLUTION_HZ / 1000000),
};
static DRAM_ATTR const rmt_symbol_word_t s_ws2812_one = {
    ///< Logical 1: T1H=0.9 µs, T1L=0.3 µs
    .level0    = 1,
    .duration0 = (uint32_t)(0.9f * RMT_RESOLUTION_HZ / 1000000),
    .level1    = 0,
    .duration1 = (uint32_t)(0.3f * RMT_RESOLUTION_HZ / 1000000),
};
static DRAM_ATTR const rmt_symbol_word_t s_ws2812_reset = {
    ///< Reset pulse: 50 µs low
    .level0    = 0,
    .duration0 = RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
    .level1    = 0,
    .duration1 = RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
};

/*!
    \brief  RMT encoder: turns pixel bytes into WS2812 pulses.
*/
static IRAM_ATTR size_t encoder_callback(const void *data, size_t data_size, size_t symbols_written,
                                         size_t symbols_free, rmt_symbol_word_t *symbols,
                                         bool *done, void *arg)
{
    if (symbols_free < 8)
    {
        return 0;
    }

    size_t data_pos      = symbols_written / 8;
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
    *done      = 1;
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

    hue           = ((hue % 360) + 360) % 360;
    int region    = hue / 60;
    int remainder = (hue - region * 60) * 255 / 60;

    uint8_t p = (uint16_t)val * (255 - sat) >> 8;
    uint8_t q = (uint16_t)val * (255 - ((uint16_t)sat * remainder >> 8)) >> 8;
    uint8_t t = (uint16_t)val * (255 - ((uint16_t)sat * (255 - remainder) >> 8)) >> 8;

    switch (region)
    {
        case 0:
            color.red   = val;
            color.green = t;
            color.blue  = p;
            break;
        case 1:
            color.red   = q;
            color.green = val;
            color.blue  = p;
            break;
        case 2:
            color.red   = p;
            color.green = val;
            color.blue  = t;
            break;
        case 3:
            color.red   = p;
            color.green = q;
            color.blue  = val;
            break;
        case 4:
            color.red   = t;
            color.green = p;
            color.blue  = val;
            break;
        default:
            color.red   = val;
            color.green = p;
            color.blue  = q;
            break;
    }
    return color;
}

/*!
    \brief  Write one pixel to the pixel buffer.
*/
static void wt_set_led_buf(int index, wt_segd_color_t color)
{
    s_pixels[index * 3 + 0] = color.green;
    s_pixels[index * 3 + 1] = color.red;
    s_pixels[index * 3 + 2] = color.blue;
}

/*!
    \brief  Read one pixel from the pixel buffer.
*/
static wt_segd_color_t wt_get_led_buf(int index)
{
    wt_segd_color_t color;
    color.green = s_pixels[index * 3 + 0];
    color.red   = s_pixels[index * 3 + 1];
    color.blue  = s_pixels[index * 3 + 2];
    return color;
}

/// LEDs lit in the current frame: the effect only colors these.
static bool s_lit[WT_SEGD_MAX_TOTAL_LEDS];

/*!
    \brief  Draw the two LEDs of a segment, or of the colon: color_on scaled by
            the brightness when on, color_off when off.
*/
static void render_led_pair(int led_offset, bool on, const wt_segd_request_t *req)
{
    s_lit[led_offset]     = on;
    s_lit[led_offset + 1] = on;
    wt_segd_color_t color = req->color_off;
    if (on)
    {
        color.red   = (uint8_t)((uint16_t)req->color_on.red * req->intensity >> 8);
        color.green = (uint8_t)((uint16_t)req->color_on.green * req->intensity >> 8);
        color.blue  = (uint8_t)((uint16_t)req->color_on.blue * req->intensity >> 8);
    }
    wt_set_led_buf(led_offset, color);
    wt_set_led_buf(led_offset + 1, color);
}

// Position data for the 58 LEDs of the digit board, used only by the
// background effects. Extra LEDs get a generated ring layout
// (build_led_layout()).
static const uint8_t k_coordsX_base[WT_SEGD_TOTAL_LEDS] = {
    242, 230, 217, 217, 230, 242, 255, 255, 255, 255, 242, 230, 217, 217, 179,
    166, 153, 153, 166, 179, 191, 191, 191, 191, 179, 166, 153, 153, 128, 128,
    89,  77,  64,  64,  77,  89,  102, 102, 102, 102, 89,  77,  64,  64,  26,
    13,  0,   0,   13,  26,  38,  38,  38,  38,  26,  13,  0,   0};
static const uint8_t k_coordsY_base[WT_SEGD_TOTAL_LEDS] = {
    128, 128, 85, 43, 0, 0,  43, 85,  170, 213, 255, 255, 213, 170, 128,
    128, 85,  43, 0,  0, 43, 85, 170, 213, 255, 255, 213, 170, 170, 85,
    128, 128, 85, 43, 0, 0,  43, 85,  170, 213, 255, 255, 213, 170, 128,
    128, 85,  43, 0,  0, 43, 85, 170, 213, 255, 255, 213, 170};
static const uint8_t k_angles_base[WT_SEGD_TOTAL_LEDS] = {
    125, 125, 118, 113, 110, 112, 117, 121, 130, 134, 139, 141, 137, 131, 122,
    119, 96,  86,  89,  96,  107, 114, 132, 141, 153, 159, 159, 141, 223, 51,
    6,   4,   11,  17,  27,  32,  32,  22,  247, 233, 230, 234, 244, 251, 2,
    2,   6,   9,   14,  16,  13,  8,   252, 247, 243, 245, 249, 253};
static const uint8_t k_radii_base[WT_SEGD_TOTAL_LEDS] = {
    201, 178, 158, 165, 196, 217, 232, 227, 225, 227, 209, 187, 158, 154, 84,
    60,  50,  69,  102, 117, 122, 112, 107, 112, 102, 84,  50,  37,  17,  37,
    84,  107, 135, 143, 135, 117, 84,  69,  60,  69,  102, 122, 135, 130, 201,
    225, 251, 255, 239, 217, 187, 181, 178, 181, 209, 232, 251, 248};

// Layout tables read by the effects: the digit board's data followed by
// the generated entries for any extra LEDs.
static uint8_t coordsX[WT_SEGD_MAX_TOTAL_LEDS];
static uint8_t coordsY[WT_SEGD_MAX_TOTAL_LEDS];
static uint8_t angles[WT_SEGD_MAX_TOTAL_LEDS];
static uint8_t radii[WT_SEGD_MAX_TOTAL_LEDS];

/*!
    \brief  Build the layout tables for led_count LEDs: the digit board keeps
            its measured positions, and extra LEDs are spread evenly
            around an outer ring.
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
        float frac     = (float)i / (float)extra_count;
        float theta    = frac * 2.0f * (float)M_PI;
        uint8_t radius = 220;

        angles[WT_SEGD_TOTAL_LEDS + i]  = (uint8_t)(frac * 255.0f);
        radii[WT_SEGD_TOTAL_LEDS + i]   = radius;
        coordsX[WT_SEGD_TOTAL_LEDS + i] = (uint8_t)(128.0f + 100.0f * cosf(theta));
        coordsY[WT_SEGD_TOTAL_LEDS + i] = (uint8_t)(128.0f + 100.0f * sinf(theta));
    }
}

static int t_rain = 0;

/*!
    \brief  Background effect: rainbow rotating around the display.
*/
static void effect_rainbow_ring(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int hue = angles[i] * 2 + t_rain;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, 180);
        wt_set_led_buf(i, c);
    }

    // Hue repeats every 360, so wrapping here changes nothing visible and
    // keeps the counter from overflowing.
    t_rain = (t_rain + 2) % 360;
}

static int t_ripple = 0;

/*!
    \brief  Background effect: blue waves moving outward.
*/
static void effect_ripple(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int wave = (sin((radii[i] + t_ripple) * 0.08) + 1.0) * 127;

        wt_segd_color_t c = wt_hsv_to_rgb(160, 255, wave); // blue tones
        wt_set_led_buf(i, c);
    }

    // The sine period is not an integer, so the counter cannot wrap
    // seamlessly; it is reset at a large bound to avoid overflow.
    t_ripple += 3;
    if (t_ripple >= 1000000000)
        t_ripple = 0;
}

static int t_galaxy = 0;

/*!
    \brief  Background effect: slowly turning colored spiral.
*/
static void effect_galaxy(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int hue = angles[i] * 4 + t_galaxy;

        int brightness = (sin((radii[i] * 0.1 + t_galaxy * 0.05)) + 1.0) * 127;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 200, brightness);
        wt_set_led_buf(i, c);
    }

    // Reset at a large bound to avoid overflow.
    t_galaxy += 2;
    if (t_galaxy >= 1000000000)
        t_galaxy = 0;
}

static int t_flow = 0;

/*!
    \brief  Background effect: color gradient flowing diagonally.
*/
static void effect_xy_flow(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int hue = coordsX[i] + coordsY[i] + t_flow;

        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, 200);
        wt_set_led_buf(i, c);
    }

    // Hue repeats every 360.
    t_flow = (t_flow + 1) % 360;
}

static int t_wave = 0;

/*!
    \brief  Background effect: a red ring expanding from the center.
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
    \brief  Repeatable 8-bit pseudo-random value for a seed, used to vary the
            effects from LED to LED.
*/
static uint8_t wt_pseudo_rand8(uint32_t seed)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return (uint8_t)(seed & 0xFFu);
}

static uint32_t t_twinkle = 0;

/*!
    \brief  Background effect: scattered white sparkles.
*/
static void effect_twinkle(int led_count)
{
    uint32_t bucket = t_twinkle / 6u;

    for (int i = 0; i < led_count; i++)
    {
        uint8_t r      = wt_pseudo_rand8((uint32_t)i * 2654435761u + bucket);
        uint8_t bright = (r > 235u) ? (uint8_t)((r - 235u) * 12u) : 0u;

        wt_segd_color_t c = wt_hsv_to_rgb(0, 0, bright); // white
        wt_set_led_buf(i, c);
    }

    t_twinkle++;
    if (t_twinkle >= 1000000000u)
        t_twinkle = 0;
}

static int t_comet = 0;

/*!
    \brief  Background effect: a bright head with a fading tail circling
            the display.
*/
static void effect_comet(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int diff = abs((int)angles[i] - t_comet);
        if (diff > 128)
            diff = 256 - diff;

        uint8_t bright = (diff < 40) ? (uint8_t)(255 - diff * 6) : 0;

        wt_segd_color_t c = wt_hsv_to_rgb(200, 255, bright); // cyan-blue comet
        wt_set_led_buf(i, c);
    }

    // Angles use a 0-255 scale, so the position wraps at 256.
    t_comet = (t_comet + 4) % 256;
}

static int t_fire = 0;

/*!
    \brief  Background effect: flickering fire, hottest at the center.
*/
static void effect_fire(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        uint8_t flicker = wt_pseudo_rand8((uint32_t)i * 40503u + (uint32_t)t_fire);
        int heat        = (255 - radii[i]) + (flicker / 4) - 32;
        if (heat < 0)
            heat = 0;
        if (heat > 255)
            heat = 255;

        int hue           = 10 + heat / 12;
        wt_segd_color_t c = wt_hsv_to_rgb(hue, 255, (uint8_t)heat);
        wt_set_led_buf(i, c);
    }

    // Reset at a large bound to avoid overflow.
    t_fire += 3;
    if (t_fire >= 1000000000)
        t_fire = 0;
}

static uint32_t t_fx = 0; ///< Frame counter for the effects below

/*!
    \brief  Effect: the whole display slowly cycles through the colors.
*/
static void effect_color_cycle(int led_count)
{
    wt_segd_color_t c = wt_hsv_to_rgb((int)(t_fx % 360u), 255, 220);
    for (int i = 0; i < led_count; i++)
        wt_set_led_buf(i, c);
}

/*!
    \brief  Effect: the digits slowly pulse to white and back.
*/
static void effect_pulse(int led_count)
{
    uint8_t v = (uint8_t)((sinf((float)t_fx * 0.06f) + 1.0f) * 110.0f);
    for (int i = 0; i < led_count; i++)
        wt_set_led_buf(i, wt_hsv_to_rgb(0, 0, v));
}

/*!
    \brief  Effect: two quick red beats, then a pause.
*/
static void effect_heartbeat(int led_count)
{
    uint32_t p = t_fx % 60u; // 1.2 s cycle
    uint8_t v  = (p < 6u)                ? (uint8_t)(255u - p * 40u)
                 : (p >= 10u && p < 16u) ? (uint8_t)(255u - (p - 10u) * 40u)
                                         : 0u;
    for (int i = 0; i < led_count; i++)
        wt_set_led_buf(i, wt_hsv_to_rgb(350, 255, v));
}

/*!
    \brief  Effect: flowing plasma of mixed colors.
*/
static void effect_plasma(int led_count)
{
    float t = (float)t_fx * 0.05f;
    for (int i = 0; i < led_count; i++)
    {
        float x = coordsX[i], y = coordsY[i];
        float v =
            sinf(x * 0.035f + t) + sinf(y * 0.045f - t * 1.3f) + sinf((x + y) * 0.025f + t * 0.7f);
        wt_set_led_buf(i, wt_hsv_to_rgb((int)((v + 3.0f) * 60.0f), 255, 220));
    }
}

/*!
    \brief  Effect: green, teal and violet curtains drifting sideways.
*/
static void effect_aurora(int led_count)
{
    float t = (float)t_fx * 0.03f;
    for (int i = 0; i < led_count; i++)
    {
        float x = coordsX[i], y = coordsY[i];
        int hue   = 120 + (int)((sinf(x * 0.02f + t) + 1.0f) * 70.0f); // green to violet
        uint8_t v = (uint8_t)(120.0f + (sinf(y * 0.03f + x * 0.015f + t * 1.7f) + 1.0f) * 65.0f);
        wt_set_led_buf(i, wt_hsv_to_rgb(hue, 230, v));
    }
}

/*!
    \brief  Effect: warm colors from orange at the top to magenta at the
            bottom, slowly shifting.
*/
static void effect_sunset(int led_count)
{
    int shift = (int)((sinf((float)t_fx * 0.02f) + 1.0f) * 15.0f);
    for (int i = 0; i < led_count; i++)
    {
        int hue = 330 + coordsY[i] * 60 / 255 + shift; // 330 (magenta) .. 30 (orange)
        wt_set_led_buf(i, wt_hsv_to_rgb(hue % 360, 255, 230));
    }
}

/*!
    \brief  Effect: each digit in its own color, all of them rotating.
*/
static void effect_candy(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        int hue = (coordsX[i] / 64) * 90 + (int)(t_fx % 360u);
        wt_set_led_buf(i, wt_hsv_to_rgb(hue % 360, 255, 220));
    }
}

/*!
    \brief  Effect: green drops falling down the digits.
*/
static void effect_rain(int led_count)
{
    for (int i = 0; i < led_count; i++)
    {
        // Each column falls with its own offset and speed.
        uint32_t col   = coordsX[i] / 13u;
        uint32_t speed = 4u + wt_pseudo_rand8(col * 97u) % 4u;
        int head       = (int)((t_fx * speed + wt_pseudo_rand8(col * 31u + 7u) * 2u) % 400u);
        int dist       = head - coordsY[i];
        uint8_t v      = (dist >= 0 && dist < 120) ? (uint8_t)(255 - dist * 2) : 0;
        wt_set_led_buf(i, wt_hsv_to_rgb(dist < 12 ? 100 : 120, dist < 12 ? 80 : 255, v));
    }
}

/*!
    \brief  Effect: left half red and right half blue, flashing in turn.
*/
static void effect_police(int led_count)
{
    uint32_t p    = t_fx % 50u; // 1 s cycle, two flashes per side
    bool left_on  = (p < 4u) || (p >= 8u && p < 12u);
    bool right_on = (p >= 25u && p < 29u) || (p >= 33u && p < 37u);
    for (int i = 0; i < led_count; i++)
    {
        bool left = coordsX[i] >= 128; // digit 1 has the highest X
        bool on   = left ? left_on : right_on;
        wt_set_led_buf(i, wt_hsv_to_rgb(left ? 0 : 230, 255, on ? 255 : 0));
    }
}

/*!
    \brief  Background effect that draws nothing, leaving the digits as
            rendered. This is the default.
*/
static void effect_solid(void)
{
}

/*!
    \brief  Draw the selected effect over the whole strip.
    \param[in]  mode       Effect number (see wt_led_anim_effect_from_name()).
    \param[in]  led_count  Number of LEDs to draw.
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
        case 4:
            effect_twinkle(led_count);
            break;
        case 5:
            effect_xy_flow(led_count);
            break;
        case 6:
            effect_shockwave(led_count);
            break;
        case 7:
            effect_comet(led_count);
            break;
        case 8:
            effect_fire(led_count);
            break;
        case 9:
            effect_color_cycle(led_count);
            break;
        case 10:
            effect_pulse(led_count);
            break;
        case 11:
            effect_heartbeat(led_count);
            break;
        case 12:
            effect_plasma(led_count);
            break;
        case 13:
            effect_aurora(led_count);
            break;
        case 14:
            effect_sunset(led_count);
            break;
        case 15:
            effect_candy(led_count);
            break;
        case 16:
            effect_rain(led_count);
            break;
        case 17:
            effect_police(led_count);
            break;
        default:
            break;
    }
    t_fx++;
}

/*!
    \brief  Convert an effect name such as "ripple" to its effect number.
    \param[in]  name  Effect name; NULL or an unknown name selects "solid".
    \return Effect number.
*/
uint8_t wt_led_anim_effect_from_name(const char *name)
{
    static const struct
    {
        const char *name;
        uint8_t mode;
    } k_anim_effects[] = {
        {"rainbow_ring", 0}, {"ripple", 1},     {"solid", 2},   {"galaxy", 3},  {"twinkle", 4},
        {"xy_flow", 5},      {"shockwave", 6},  {"comet", 7},   {"fire", 8},    {"color_cycle", 9},
        {"pulse", 10},       {"heartbeat", 11}, {"plasma", 12}, {"aurora", 13}, {"sunset", 14},
        {"candy", 15},       {"rain", 16},      {"police", 17},
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
    \brief  LED task: renders the display at a fixed frame rate.
    \param[in]  pvParameter  Unused.
*/
void wt_task_led(void *pvParameter)
{
    if (!wt_segd_queue)
    {
        wt_log_error("wt_segd_queue not created before wt_task_led started");
        vTaskDelete(NULL);
        return;
    }

    rmt_channel_handle_t led_chan  = NULL;
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .gpio_num          = RMT_GPIO_NUM,
        .mem_block_symbols = 64,
        .resolution_hz     = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &led_chan));

    rmt_encoder_handle_t rtm_encoder_h  = NULL;
    rmt_simple_encoder_config_t enc_cfg = {.callback = encoder_callback};
    ESP_ERROR_CHECK(rmt_new_simple_encoder(&enc_cfg, &rtm_encoder_h));
    ESP_ERROR_CHECK(rmt_enable(led_chan));

    rmt_transmit_config_t tx_config = {.loop_count = 0};

    wt_segd_request_t current = {
        .mode        = WT_SEGD_MODE_NUMBER,
        .value       = 0,
        .colon       = true,
        .colon_blink = false,
        .color_on    = WT_SEGD_GREEN,
        .color_off   = WT_SEGD_DIM,
        .intensity   = 255,
        .led_count   = WT_SEGD_TOTAL_LEDS,
    };
    wt_segd_request_t target = current;

    wt_segd_frame_t frame = {0};
    wt_segd_prepare_frame(&current, &frame);

    int applied_led_count = current.led_count;
    build_led_layout(applied_led_count);

    uint32_t tick             = 0; ///< Frame counter, used for the colon blink
    uint32_t rmt_fault_frames = 0; ///< Frames in a row that failed to send

    wt_log_info("Render loop started (%d LEDs: D1@0 D2@14 colon@28 D3@30 D4@44)",
                applied_led_count);

    TickType_t last_wake          = xTaskGetTickCount();
    const uint8_t effect_solid_id = wt_led_anim_effect_from_name("solid");

    while (1)
    {
        wt_segd_request_t new_req;
        if (xQueueReceive(wt_segd_queue, &new_req, 0) == pdTRUE)
        {
            target                 = new_req;
            uint8_t keep_intensity = current.intensity;
            current                = new_req;
            current.intensity      = keep_intensity;

            // Clamped here too: led_count sizes the pixel loops and the
            // transmit length, whoever sent the request.
            if (current.led_count < WT_SEGD_TOTAL_LEDS)
            {
                current.led_count = WT_SEGD_TOTAL_LEDS;
            }
            if (current.led_count > WT_SEGD_MAX_TOTAL_LEDS)
            {
                current.led_count = WT_SEGD_MAX_TOTAL_LEDS;
            }

            if (current.led_count != applied_led_count)
            {
                build_led_layout(current.led_count);
                applied_led_count = current.led_count;
            }
        }

        if (current.intensity < target.intensity)
        {
            int next          = current.intensity + INTENSITY_SLEW_STEP;
            current.intensity = (next > target.intensity) ? target.intensity : (uint8_t)next;
        }
        else if (current.intensity > target.intensity)
        {
            int next          = current.intensity - INTENSITY_SLEW_STEP;
            current.intensity = (next < target.intensity) ? target.intensity : (uint8_t)next;
        }

        wt_segd_prepare_frame(&current, &frame);

        bool colon_on = current.colon_blink ? ((tick / COLON_BLINK_FRAMES) % 2 == 0) : frame.colon;

        wt_segd_snapshot_t snapshot = {
            .request  = current,
            .frame    = frame,
            .colon_on = colon_on,
        };

        for (int v = 0; v < WT_SEGD_NUM_DIGITS; v++)
        {
            int led_offset = s_digit_led_start[v];
            for (int i = 0; i < WT_SEGD_SEGS_PER_DIGIT; i++)
            {
                bool seg_on = (frame.digit[v] >> s_strip_pos_to_bit[i]) & 0x01;
                render_led_pair(led_offset + i * WT_SEGD_LEDS_PER_SEG, seg_on, &current);
            }
        }
        render_led_pair(WT_SEGD_COLON_LED_OFFSET, colon_on, &current);

        // Effect: drawn over the whole strip, then kept only on the lit
        // segments, blended with color_on and scaled by the brightness, so
        // it animates the value shown without hiding it.
        // Unlit segments go back to color_off, and LEDs beyond the digit
        // board stay dark. The "solid" effect draws nothing.
        if (current.anim_effect != effect_solid_id)
        {
            run_effects(current.anim_effect, applied_led_count);
            for (int i = 0; i < applied_led_count; i++)
            {
                wt_segd_color_t c = current.color_off;
                if (i < WT_SEGD_TOTAL_LEDS && s_lit[i])
                {
                    // Where the effect is dark, color_on shows through, so a
                    // lit segment never goes black and the value stays
                    // readable; where it is bright, the effect wins.
                    wt_segd_color_t fx = wt_get_led_buf(i);
                    int peak           = fx.red > fx.green ? fx.red : fx.green;
                    peak               = peak > fx.blue ? peak : fx.blue;
                    int base           = 255 - peak;
                    int r              = fx.red + current.color_on.red * base / 255;
                    int g              = fx.green + current.color_on.green * base / 255;
                    int b              = fx.blue + current.color_on.blue * base / 255;
                    c.red              = (uint8_t)((r > 255 ? 255 : r) * current.intensity >> 8);
                    c.green            = (uint8_t)((g > 255 ? 255 : g) * current.intensity >> 8);
                    c.blue             = (uint8_t)((b > 255 ? 255 : b) * current.intensity >> 8);
                }
                else if (i >= WT_SEGD_TOTAL_LEDS)
                {
                    c = WT_SEGD_OFF;
                }
                wt_set_led_buf(i, c);
            }
        }

        // The snapshot is taken after the effect, so it matches what the
        // strip actually shows.
        for (int v = 0; v < WT_SEGD_NUM_DIGITS; v++)
        {
            int led_offset = s_digit_led_start[v];
            for (int i = 0; i < WT_SEGD_SEGS_PER_DIGIT; i++)
            {
                snapshot.digit_color[v][s_strip_pos_to_bit[i]] =
                    wt_get_led_buf(led_offset + i * WT_SEGD_LEDS_PER_SEG);
            }
        }
        snapshot.colon_color = wt_get_led_buf(WT_SEGD_COLON_LED_OFFSET);

        wt_segd_snapshot_set(&snapshot);

        // A failed transmit skips the frame; it does not reboot the device.
        esp_err_t tx_err = rmt_transmit(led_chan, rtm_encoder_h, s_pixels,
                                        (size_t)applied_led_count * 3, &tx_config);
        if (tx_err == ESP_OK)
        {
            tx_err = rmt_tx_wait_all_done(led_chan, pdMS_TO_TICKS(RMT_WAIT_TIMEOUT_MS));
        }

        // Log a persistent fault only occasionally, not on every frame.
        if (tx_err != ESP_OK)
        {
            if ((rmt_fault_frames % RMT_WARN_EVERY_FRAMES) == 0)
            {
                wt_log_warn("RMT transmit failed: %s (frame skipped)", esp_err_to_name(tx_err));
            }
            rmt_fault_frames++;
        }
        else
        {
            rmt_fault_frames = 0;
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FRAME_MS));

        tick++;
    }
}
