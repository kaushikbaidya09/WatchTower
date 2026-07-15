/*!
    \file   wt_app_log.c
    \brief  In-memory log ring buffer consumed by the web server.

    \details
    Fixed-size ring buffer guarded by a mutex; oldest entries are
    overwritten once full.
 */

#include "wt_app_log.h"
#include <string.h>
#include <stdio.h>

typedef struct
{
    char data[WT_LOG_ENTRY_LEN];
    uint32_t seq; ///< Monotonically increasing, never resets
} log_entry_t;

static log_entry_t s_buf[WT_LOG_BUF_ENTRIES];
static uint32_t s_write_idx = 0; ///< Next slot to overwrite
static uint32_t s_seq = 0;       ///< Last assigned sequence
static SemaphoreHandle_t wt_app_log_mutex = NULL;

/*!
    \brief  Initialise the ring buffer mutex. Must be called before any
            task uses wt_log_x(). Safe to call multiple times.
 */
void wt_log_init(void)
{
    if (wt_app_log_mutex == NULL)
    {
        wt_app_log_mutex = xSemaphoreCreateMutex();
    }
}

/*!
    \brief  Append a pre-formatted log line to the ring buffer. Called
            internally by the wt_log_x() macros; may also be used directly
            for injecting synthetic entries.
    \param[in]  line  Null-terminated string (truncated to WT_LOG_ENTRY_LEN-1).
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
    \brief  JSON-encode a single character into dst, returns chars written.
 */
static int json_escape_char(char *dst, size_t avail, char c)
{
    if (avail < 2)
        return 0;
    if (c == '"')
    {
        dst[0] = '\\';
        dst[1] = '"';
        return 2;
    }
    if (c == '\\')
    {
        dst[0] = '\\';
        dst[1] = '\\';
        return 2;
    }
    if (c == '\n')
    {
        dst[0] = '\\';
        dst[1] = 'n';
        return 2;
    }
    if (c == '\r')
    {
        dst[0] = '\\';
        dst[1] = 'r';
        return 2;
    }
    if (c == '\t')
    {
        dst[0] = '\\';
        dst[1] = 't';
        return 2;
    }
    dst[0] = c;
    return 1;
}

/*!
    \brief  Serialise new log entries (seq > from_seq) as a JSON object:
            { "seq": <uint32>, "entries": [ "<line>", ... ] }
    \param[in]  from_seq   Sequence number of the last entry the caller has
                           seen. Pass 0 to receive all buffered entries.
    \param[out] out_buf    Caller-allocated output buffer.
    \param[in]  buf_len    Size of out_buf in bytes.
    \param[out] next_seq   Value to pass as from_seq on the next call.
    \return                Number of new entries written into out_buf.
 */
int wt_log_read_json(uint32_t from_seq, char *out_buf,
                     size_t buf_len, uint32_t *next_seq)
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

    uint32_t cur_seq = s_seq;
    uint32_t cur_write = s_write_idx;

    /* Nothing new */
    if (cur_seq <= from_seq)
    {
        xSemaphoreGive(wt_app_log_mutex);
        snprintf(out_buf, buf_len, "{\"seq\":%lu,\"entries\":[]}", cur_seq);
        if (next_seq)
            *next_seq = from_seq;
        return 0;
    }

    size_t pos = 0;
    int count = 0;

    pos += snprintf(out_buf + pos, buf_len - pos,
                    "{\"seq\":%lu,\"entries\":[", cur_seq);

    /* Walk ring buffer oldest→newest */
    uint32_t start = (cur_write >= WT_LOG_BUF_ENTRIES)
                         ? cur_write - WT_LOG_BUF_ENTRIES
                         : 0;

    for (uint32_t i = start; i < cur_write; i++)
    {
        uint32_t idx = i % WT_LOG_BUF_ENTRIES;
        if (s_buf[idx].seq <= from_seq)
            continue;
        if (pos >= buf_len - 8)
            break; /* safety margin */

        if (count > 0 && pos < buf_len - 2)
            out_buf[pos++] = ',';
        if (pos < buf_len - 2)
            out_buf[pos++] = '"';

        const char *src = s_buf[idx].data;
        while (*src && pos < buf_len - 6)
        {
            int n = json_escape_char(out_buf + pos, buf_len - pos - 4, *src++);
            pos += n;
        }

        if (pos < buf_len - 2)
            out_buf[pos++] = '"';
        count++;
    }

    if (pos < buf_len - 2)
    {
        out_buf[pos++] = ']';
        out_buf[pos++] = '}';
    }
    out_buf[pos] = '\0';

    xSemaphoreGive(wt_app_log_mutex);

    if (next_seq)
        *next_seq = cur_seq;
    return count;
}
