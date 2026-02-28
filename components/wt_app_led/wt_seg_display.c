/*!
    \file   wt_seg_display.c
    \brief  7-segment character encoding and frame preparation.

    Implements wt_segd_char() and wt_segd_prepare_frame().
    All lookup tables are file-local; the public API is declared in
    wt_seg_display.h.
 */

#include "wt_seg_display.h"
#include <string.h>
#include <time.h>

/*!
    Digit lookup table  (A=bit0, B=bit1, C=bit2, D=bit3, E=bit4, F=bit5, G=bit6)
 */
static const uint8_t s_digits[10] = {
    [0] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    [1] = WT_SEGD_B | WT_SEGD_C,
    [2] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    [3] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_G,
    [4] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_F | WT_SEGD_G,
    [5] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    [6] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [7] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C,
    [8] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    [9] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
};

/*!
    Uppercase letter table.
    Best-effort representations; some letters are ambiguous on 7 segments.
 */
static const uint8_t s_upper[26] = {
    ['A' - 'A'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['B' - 'A'] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G, /* looks like b */
    ['C' - 'A'] = WT_SEGD_A | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    ['D' - 'A'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G, /* looks like d */
    ['E' - 'A'] = WT_SEGD_A | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['F' - 'A'] = WT_SEGD_A | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['G' - 'A'] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    ['H' - 'A'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['I' - 'A'] = WT_SEGD_E | WT_SEGD_F,
    ['J' - 'A'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,
    ['K' - 'A'] = WT_SEGD_A | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G, /* approx */
    ['L' - 'A'] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    ['M' - 'A'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F, /* approx, like A */
    ['N' - 'A'] = WT_SEGD_C | WT_SEGD_E | WT_SEGD_G,                         /* looks like n */
    ['O' - 'A'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    ['P' - 'A'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['Q' - 'A'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_F | WT_SEGD_G,
    ['R' - 'A'] = WT_SEGD_E | WT_SEGD_G,                                     /* looks like r */
    ['S' - 'A'] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G, /* same as 5 */
    ['T' - 'A'] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,             /* looks like t */
    ['U' - 'A'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F,
    ['V' - 'A'] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,                         /* lower half only */
    ['W' - 'A'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F, /* approx, like U */
    ['X' - 'A'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G, /* like H */
    ['Y' - 'A'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    ['Z' - 'A'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G, /* same as 2 */
};

/*!
    Lowercase letter table.
    Where lowercase differs meaningfully from uppercase a distinct glyph is
    used; otherwise falls back to the uppercase approximation.
 */
static const uint8_t s_lower[26] = {
    ['a' - 'a'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    ['b' - 'a'] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['c' - 'a'] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    ['d' - 'a'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    ['e' - 'a'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['f' - 'a'] = WT_SEGD_A | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['g' - 'a'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    ['h' - 'a'] = WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['i' - 'a'] = WT_SEGD_E,
    ['j' - 'a'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,
    ['k' - 'a'] = WT_SEGD_A | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G, /* approx */
    ['l' - 'a'] = WT_SEGD_E | WT_SEGD_F,
    ['m' - 'a'] = WT_SEGD_C | WT_SEGD_E | WT_SEGD_G, /* approx, like n */
    ['n' - 'a'] = WT_SEGD_C | WT_SEGD_E | WT_SEGD_G,
    ['o' - 'a'] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G,
    ['p' - 'a'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['q' - 'a'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_F | WT_SEGD_G,
    ['r' - 'a'] = WT_SEGD_E | WT_SEGD_G,
    ['s' - 'a'] = WT_SEGD_A | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G, /* same as 5/S */
    ['t' - 'a'] = WT_SEGD_D | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G,
    ['u' - 'a'] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,
    ['v' - 'a'] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,                         /* same as u */
    ['w' - 'a'] = WT_SEGD_C | WT_SEGD_D | WT_SEGD_E,                         /* approx */
    ['x' - 'a'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_E | WT_SEGD_F | WT_SEGD_G, /* like H */
    ['y' - 'a'] = WT_SEGD_B | WT_SEGD_C | WT_SEGD_D | WT_SEGD_F | WT_SEGD_G,
    ['z' - 'a'] = WT_SEGD_A | WT_SEGD_B | WT_SEGD_D | WT_SEGD_E | WT_SEGD_G, /* same as 2/Z */
};

/*!
    Symbol definitions  (file-local).
    Accessed only through wt_segd_char(); not exposed in the header.
 */
#define S_DASH WT_SEGD_G
#define S_UNDERSCORE WT_SEGD_D
#define S_EQUALS (WT_SEGD_D | WT_SEGD_G)
#define S_TILDE (WT_SEGD_A | WT_SEGD_G)
#define S_DEGREE (WT_SEGD_A | WT_SEGD_B | WT_SEGD_F | WT_SEGD_G)
#define S_BRACKET_L (WT_SEGD_A | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F)
#define S_BRACKET_R (WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D)
#define S_PAREN_L (WT_SEGD_A | WT_SEGD_D | WT_SEGD_E | WT_SEGD_F)
#define S_PAREN_R (WT_SEGD_A | WT_SEGD_B | WT_SEGD_C | WT_SEGD_D)
#define S_APOSTROPHE WT_SEGD_F
#define S_QUOTE (WT_SEGD_B | WT_SEGD_F)
#define S_QUESTION (WT_SEGD_A | WT_SEGD_B | WT_SEGD_E | WT_SEGD_G)
#define S_EXCLAIM (WT_SEGD_B | WT_SEGD_C)
#define S_PLUS (WT_SEGD_B | WT_SEGD_C | WT_SEGD_F | WT_SEGD_G)
#define S_SPACE WT_SEGD_NONE

/*!
    \brief  Return the segment bitmask for a single ASCII character.

    \param[in]  c  ASCII character to look up.
    \return     Segment bitmask (bits WT_SEGD_A … WT_SEGD_G).
                Returns S_DASH for any character without a defined glyph.
 */
uint8_t wt_segd_char(char c)
{
    if (c >= '0' && c <= '9')
        return s_digits[c - '0'];
    if (c >= 'A' && c <= 'Z')
        return s_upper[c - 'A'];
    if (c >= 'a' && c <= 'z')
        return s_lower[c - 'a'];

    switch (c)
    {
    case '-':
        return S_DASH;
    case '_':
        return S_UNDERSCORE;
    case '=':
        return S_EQUALS;
    case '~':
        return S_TILDE;
    case '[':
        return S_BRACKET_L;
    case ']':
        return S_BRACKET_R;
    case '(':
        return S_PAREN_L;
    case ')':
        return S_PAREN_R;
    case '\'':
        return S_APOSTROPHE;
    case '"':
        return S_QUOTE;
    case '?':
        return S_QUESTION;
    case '!':
        return S_EXCLAIM;
    case '+':
        return S_PLUS;
    case ' ':
        return S_SPACE;
    default:
        return S_DASH;
    }
}

/*!
    \brief  Convert a wt_segd_request_t into a wt_segd_frame_t.

    Digit ordering in the output frame is always left-to-right visually:
      - frame->digit[0] = D4 (leftmost)
      - frame->digit[3] = D1 (rightmost)

    The LED render loop maps these visual positions to physical strip offsets.

    \param[in]  req    Incoming display request.
    \param[out] frame  Populated with per-digit segment masks and colon state.
 */
void wt_segd_prepare_frame(const wt_segd_request_t *req, wt_segd_frame_t *frame)
{
    switch (req->mode)
    {

    case WT_SEGD_MODE_NUMBER:
    {
        /* Integer 0-9999 split into individual decimal digits.
           digit[0]=thousands  digit[1]=hundreds  digit[2]=tens  digit[3]=units */
        int v = req->value;
        if (v < 0)
            v = 0;
        if (v > 9999)
            v = 9999;
        frame->digit[0] = s_digits[v / 1000];
        frame->digit[1] = s_digits[(v / 100) % 10];
        frame->digit[2] = s_digits[(v / 10) % 10];
        frame->digit[3] = s_digits[v % 10];
        break;
    }

    case WT_SEGD_MODE_TIME:
    {
        /* Read current system time and display as HH:MM.
           req->value is ignored in this mode. */
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        frame->digit[0] = s_digits[t.tm_hour / 10];
        frame->digit[1] = s_digits[t.tm_hour % 10];
        frame->digit[2] = s_digits[t.tm_min / 10];
        frame->digit[3] = s_digits[t.tm_min % 10];
        break;
    }

    case WT_SEGD_MODE_TEXT:
    {
        /* text[0]=D4(left) … text[3]=D1(right).
           A null character in any position renders as blank. */
        for (int i = 0; i < WT_SEGD_NUM_DIGITS; i++)
        {
            frame->digit[i] = req->text[i]
                                  ? wt_segd_char(req->text[i])
                                  : WT_SEGD_NONE;
        }
        break;
    }

    case WT_SEGD_MODE_RAW:
    {
        /* raw[0]=D4(left) … raw[3]=D1(right).
           Caller supplies bitmasks directly using WT_SEGD_A … WT_SEGD_G. */
        for (int i = 0; i < WT_SEGD_NUM_DIGITS; i++)
        {
            frame->digit[i] = req->raw[i];
        }
        break;
    }

    default:
        for (int i = 0; i < WT_SEGD_NUM_DIGITS; i++)
        {
            frame->digit[i] = WT_SEGD_NONE;
        }
        break;
    }

    frame->colon = req->colon;
}