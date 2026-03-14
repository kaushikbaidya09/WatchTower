#include "wt_seg_display.h"
#include <string.h>
#include <time.h>

typedef enum
{
    SEGD_SYM_SPACE = 0,
    SEGD_SYM_COLON,
    SEGD_SYM_DASH,
    SEGD_SYM_UNDERSCORE,
    SEGD_SYM_EQUAL,
    SEGD_SYM_DEGREE,

    SEGD_SYM_DIGIT_0,
    SEGD_SYM_DIGIT_1,
    SEGD_SYM_DIGIT_2,
    SEGD_SYM_DIGIT_3,
    SEGD_SYM_DIGIT_4,
    SEGD_SYM_DIGIT_5,
    SEGD_SYM_DIGIT_6,
    SEGD_SYM_DIGIT_7,
    SEGD_SYM_DIGIT_8,
    SEGD_SYM_DIGIT_9,

    SEGD_SYM_UPPER_A,
    SEGD_SYM_UPPER_B,
    SEGD_SYM_UPPER_C,
    SEGD_SYM_UPPER_D,
    SEGD_SYM_UPPER_E,
    SEGD_SYM_UPPER_F,
    SEGD_SYM_UPPER_G,
    SEGD_SYM_UPPER_H,
    SEGD_SYM_UPPER_I,
    SEGD_SYM_UPPER_J,
    SEGD_SYM_UPPER_K,
    SEGD_SYM_UPPER_L,
    SEGD_SYM_UPPER_M,
    SEGD_SYM_UPPER_N,
    SEGD_SYM_UPPER_O,
    SEGD_SYM_UPPER_P,
    SEGD_SYM_UPPER_Q,
    SEGD_SYM_UPPER_R,
    SEGD_SYM_UPPER_S,
    SEGD_SYM_UPPER_T,
    SEGD_SYM_UPPER_U,
    SEGD_SYM_UPPER_V,
    SEGD_SYM_UPPER_W,
    SEGD_SYM_UPPER_X,
    SEGD_SYM_UPPER_Y,
    SEGD_SYM_UPPER_Z,

    SEGD_SYM_LOWER_A,
    SEGD_SYM_LOWER_B,
    SEGD_SYM_LOWER_C,
    SEGD_SYM_LOWER_D,
    SEGD_SYM_LOWER_E,
    SEGD_SYM_LOWER_F,
    SEGD_SYM_LOWER_G,
    SEGD_SYM_LOWER_H,
    SEGD_SYM_LOWER_I,
    SEGD_SYM_LOWER_J,
    SEGD_SYM_LOWER_K,
    SEGD_SYM_LOWER_L,
    SEGD_SYM_LOWER_M,
    SEGD_SYM_LOWER_N,
    SEGD_SYM_LOWER_O,
    SEGD_SYM_LOWER_P,
    SEGD_SYM_LOWER_Q,
    SEGD_SYM_LOWER_R,
    SEGD_SYM_LOWER_S,
    SEGD_SYM_LOWER_T,
    SEGD_SYM_LOWER_U,
    SEGD_SYM_LOWER_V,
    SEGD_SYM_LOWER_W,
    SEGD_SYM_LOWER_X,
    SEGD_SYM_LOWER_Y,
    SEGD_SYM_LOWER_Z,

    SEGD_SYM_MAX

} wt_segd_symbol_t;

/*!
    7 Segment display symbols lookup table.  (A=bit0, B=bit1, C=bit2, D=bit3, E=bit4, F=bit5, G=bit6)
 */
static const uint8_t wt_segd_symbol[SEGD_SYM_MAX] = {

    [SEGD_SYM_SPACE] = WT_SEGD_NONE,
    [SEGD_SYM_COLON] = WT_SEGD_NONE,
    [SEGD_SYM_DASH] = WT_SEGD_G,
    [SEGD_SYM_UNDERSCORE] = WT_SEGD_D,
    [SEGD_SYM_EQUAL] = WT_SEGD_G | WT_SEGD_D,
    [SEGD_SYM_DEGREE] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_F | WT_SEGD_G,

    // Digits
    [SEGD_SYM_DIGIT_0] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [SEGD_SYM_DIGIT_1] = WT_SEGD_B | WT_SEGD_C,
    [SEGD_SYM_DIGIT_2] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_DIGIT_3] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_G,
    [SEGD_SYM_DIGIT_4] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_DIGIT_5] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_DIGIT_6] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_DIGIT_7] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C,
    [SEGD_SYM_DIGIT_8] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_DIGIT_9] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,

    // Uppercase letters
    [SEGD_SYM_UPPER_A] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_B] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_C] = WT_SEGD_A | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [SEGD_SYM_UPPER_D] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_UPPER_E] = WT_SEGD_A | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_F] = WT_SEGD_A | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_G] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [SEGD_SYM_UPPER_H] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_I] = WT_SEGD_B | WT_SEGD_C,
    [SEGD_SYM_UPPER_J] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,
    [SEGD_SYM_UPPER_K] = WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_L] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [SEGD_SYM_UPPER_M] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_E,
    [SEGD_SYM_UPPER_N] = WT_SEGD_C | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_UPPER_O] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [SEGD_SYM_UPPER_P] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_Q] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_R] = WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_UPPER_S] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_T] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_U] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [SEGD_SYM_UPPER_V] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,
    [SEGD_SYM_UPPER_W] = WT_SEGD_B | WT_SEGD_D | WT_SEGD_F,
    [SEGD_SYM_UPPER_X] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_Y] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_UPPER_Z] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,

    // Lowercase letters
    [SEGD_SYM_LOWER_A] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_LOWER_B] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_C] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_LOWER_D] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_LOWER_E] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_F] = WT_SEGD_A | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_G] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_H] = WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_I] = WT_SEGD_C,
    [SEGD_SYM_LOWER_J] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D,
    [SEGD_SYM_LOWER_K] = WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_L] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [SEGD_SYM_LOWER_M] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_E,
    [SEGD_SYM_LOWER_N] = WT_SEGD_C | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_LOWER_O] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_LOWER_P] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_Q] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_R] = WT_SEGD_E | WT_SEGD_G,
    [SEGD_SYM_LOWER_S] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_T] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_U] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,
    [SEGD_SYM_LOWER_V] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,
    [SEGD_SYM_LOWER_W] = WT_SEGD_B | WT_SEGD_D | WT_SEGD_F,
    [SEGD_SYM_LOWER_X] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_Y] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    [SEGD_SYM_LOWER_Z] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
};

/*!
    \brief  Return the segment bitmask for a single ASCII character.
 */
uint8_t wt_segd_char(char ch)
{
    /* digits */
    if (ch >= '0' && ch <= '9')
    {
        return wt_segd_symbol[SEGD_SYM_DIGIT_0 + (ch - '0')];
    }
    else if (ch >= 'A' && ch <= 'Z')
    {
        return wt_segd_symbol[SEGD_SYM_UPPER_A + (ch - 'A')];
    }
    else if (ch >= 'a' && ch <= 'z')
    {
        return wt_segd_symbol[SEGD_SYM_LOWER_A + (ch - 'a')];
    }
    else
    {
        switch (ch)
        {
        case ':':
            return wt_segd_symbol[SEGD_SYM_COLON];
        case '-':
            return wt_segd_symbol[SEGD_SYM_DASH];
        case '_':
            return wt_segd_symbol[SEGD_SYM_UNDERSCORE];
        case '=':
            return wt_segd_symbol[SEGD_SYM_EQUAL];
        case ' ':
            return wt_segd_symbol[SEGD_SYM_SPACE];
        default:
            return wt_segd_symbol[SEGD_SYM_SPACE];
        }
    }
}

/*!
    \brief  Convert a wt_segd_request_t into a wt_segd_frame_t.
 */
void wt_segd_prepare_frame(const wt_segd_request_t *req, wt_segd_frame_t *frame)
{
    switch (req->mode)
    {
    case WT_SEGD_MODE_NUMBER:
        int val = req->value;
        val = val < 0      ? 0
              : val > 9999 ? 9999
                           : val;
        frame->digit[0] = wt_segd_char('0' + (val / 1000));
        frame->digit[1] = wt_segd_char('0' + ((val / 100) % 10));
        frame->digit[2] = wt_segd_char('0' + ((val / 10) % 10));
        frame->digit[3] = wt_segd_char('0' + (val % 10));
        break;

    case WT_SEGD_MODE_TIME:
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        frame->digit[0] = wt_segd_char('0' + (t.tm_hour / 10));
        frame->digit[1] = wt_segd_char('0' + (t.tm_hour % 10));
        frame->digit[2] = wt_segd_char('0' + (t.tm_min / 10));
        frame->digit[3] = wt_segd_char('0' + (t.tm_min % 10));
        break;

    case WT_SEGD_MODE_TEXT:
        for (int i = 0; i < WT_SEGD_NUM_DIGITS; i++)
        {
            frame->digit[i] = req->text[i]
                                  ? wt_segd_char(req->text[i])
                                  : WT_SEGD_NONE;
        }
        break;

    case WT_SEGD_MODE_RAW:
        for (int i = 0; i < WT_SEGD_NUM_DIGITS; i++)
        {
            frame->digit[i] = req->raw[i];
        }
        break;

    default:
        for (int i = 0; i < WT_SEGD_NUM_DIGITS; i++)
        {
            frame->digit[i] = WT_SEGD_NONE;
        }
        break;
    }

    frame->colon = req->colon;
}