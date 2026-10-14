//=============================================================================
/*!
    \file    wt_app_log.c
    \brief   Log buffer read by the web server.
*/
//=============================================================================

// Fixed-size ring buffer guarded by a mutex; the oldest entry is
// overwritten when it is full.

#include "wt_app_log.h"
#include <inttypes.h>
#include <string.h>
#include <stdio.h>

typedef struct
{
    char data[WT_LOG_ENTRY_LEN];
    uint32_t seq; ///< Sequence number; only ever increases
} log_entry_t;

static log_entry_t s_buf[WT_LOG_BUF_ENTRIES];
static uint32_t s_write_idx               = 0; ///< Next slot to overwrite
static uint32_t s_seq                     = 0; ///< Last sequence number used
static SemaphoreHandle_t wt_app_log_mutex = NULL;

/*!
    \brief  Create the log mutex. Call before the first log call.
*/
void wt_log_init(void)
{
    if (wt_app_log_mutex == NULL)
    {
        wt_app_log_mutex = xSemaphoreCreateMutex();
    }
}

/*!
    \brief  Add a line to the log buffer. Used by the wt_log_x() macros.
    \param[in]  line  Text to store; cut to WT_LOG_ENTRY_LEN - 1 characters.
*/
void wt_log_append(const char *line)
{
    if (!wt_app_log_mutex || !line)
        return;

    if (xSemaphoreTake(wt_app_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        uint32_t idx = s_write_idx % WT_LOG_BUF_ENTRIES;
        strlcpy(s_buf[idx].data, line, WT_LOG_ENTRY_LEN);
        s_buf[idx].seq = ++s_seq;
        s_write_idx++;
        xSemaphoreGive(wt_app_log_mutex);
    }
}

/*!
    \brief  Letter that follows the backslash when c is escaped in a JSON
            string ('"', '\\', 'n', 'r', 't'), or 0 if c is written as is.
*/
static char json_escape_code(char c)
{
    switch (c)
    {
        case '"':
            return '"';
        case '\\':
            return '\\';
        case '\n':
            return 'n';
        case '\r':
            return 'r';
        case '\t':
            return 't';
        default:
            return 0;
    }
}

/*!
    \brief  Write the log lines newer than from_seq as JSON:
            { "seq": <number>, "entries": [ "<line>", ... ] }
    \param[in]  from_seq   Sequence number of the last line already read; 0 for all.
    \param[out] out_buf    Output buffer.
    \param[in]  buf_len    Size of out_buf in bytes.
    \param[out] next_seq   Receives the value to pass as from_seq next time.
    \return Number of lines written.
*/
int wt_log_read_json(uint32_t from_seq, char *out_buf, size_t buf_len, uint32_t *next_seq)
{
    if (!out_buf || buf_len < 16)
        return 0;

    if (!wt_app_log_mutex)
    {
        snprintf(out_buf, buf_len, "{\"seq\":0,\"entries\":[]}");
        if (next_seq)
            *next_seq = 0;
        return 0;
    }

    xSemaphoreTake(wt_app_log_mutex, portMAX_DELAY);

    uint32_t cur_seq   = s_seq;
    uint32_t cur_write = s_write_idx;

    if (cur_seq <= from_seq)
    {
        xSemaphoreGive(wt_app_log_mutex);
        snprintf(out_buf, buf_len, "{\"seq\":%" PRIu32 ",\"entries\":[]}", cur_seq);
        if (next_seq)
            *next_seq = from_seq;
        return 0;
    }

    size_t pos        = 0;
    int count         = 0;
    uint32_t last_seq = from_seq; ///< Newest entry written to out_buf

    pos += snprintf(out_buf + pos, buf_len - pos, "{\"seq\":%" PRIu32 ",\"entries\":[", cur_seq);

    // Oldest entry first.
    uint32_t start = (cur_write >= WT_LOG_BUF_ENTRIES) ? cur_write - WT_LOG_BUF_ENTRIES : 0;

    for (uint32_t i = start; i < cur_write; i++)
    {
        uint32_t idx = i % WT_LOG_BUF_ENTRIES;
        if (s_buf[idx].seq <= from_seq)
            continue;
        // Only add an entry that fits completely. The rest is returned by
        // the next call, since next_seq stops at the last entry written.
        size_t escaped_len = 0;
        for (const char *p = s_buf[idx].data; *p; p++)
            escaped_len += json_escape_code(*p) ? 2 : 1;
        if ((pos + escaped_len + 8) > buf_len)
            break;

        if (count > 0 && pos < buf_len - 2)
            out_buf[pos++] = ',';
        if (pos < buf_len - 2)
            out_buf[pos++] = '"';

        const char *src = s_buf[idx].data;
        for (; *src && pos < buf_len - 6; src++)
        {
            char code = json_escape_code(*src);
            if (code)
            {
                out_buf[pos++] = '\\';
                out_buf[pos++] = code;
            }
            else
            {
                // Other control characters are not allowed in a JSON string.
                out_buf[pos++] = ((unsigned char)*src < 0x20) ? ' ' : *src;
            }
        }

        if (pos < buf_len - 2)
            out_buf[pos++] = '"';
        count++;
        last_seq = s_buf[idx].seq;
    }

    if (pos < buf_len - 2)
    {
        out_buf[pos++] = ']';
        out_buf[pos++] = '}';
    }
    out_buf[pos] = '\0';

    xSemaphoreGive(wt_app_log_mutex);

    if (next_seq)
        *next_seq = last_seq;
    return count;
}
