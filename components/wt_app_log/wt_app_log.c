/*!
    \file   wt_app_log.c
    \brief  In-memory log ring buffer consumed by the web server.
 */
#include "wt_app_log.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/*  Internal ring buffer                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    char data[WT_LOG_ENTRY_LEN];
    uint32_t seq; /*!< Monotonically increasing, never resets */
} log_entry_t;

static log_entry_t s_buf[WT_LOG_BUF_ENTRIES];
static uint32_t s_write_idx = 0; /*!< Next slot to overwrite */
static uint32_t s_seq = 0;       /*!< Last assigned sequence  */
static SemaphoreHandle_t wt_app_log_mutex = NULL;

void wt_log_init(void)
{
    if (wt_app_log_mutex == NULL)
    {
        wt_app_log_mutex = xSemaphoreCreateMutex();
    }
}

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

/* JSON-encode a single character into dst, returns chars written */
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
    uint32_t total = (cur_write < WT_LOG_BUF_ENTRIES)
                         ? cur_write
                         : (uint32_t)WT_LOG_BUF_ENTRIES;
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
