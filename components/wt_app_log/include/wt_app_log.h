/*!
    \file   wt_app_log.h
    \brief  Application logging — console output + in-memory ring buffer.

    Every APPLOG_x() call:
      1. Prints the formatted line to stdout (same as before).
      2. Appends it to an internal ring buffer so the web server can
         serve recent log entries via GET /api/logs?seq=N.

    Call wt_log_init() once before any task that uses APPLOG.
 */
#ifndef WT_APP_LOG_H
#define WT_APP_LOG_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

/* ------------------------------------------------------------------ */
/*  Ring buffer configuration                                           */
/* ------------------------------------------------------------------ */
#define WT_LOG_BUF_ENTRIES 128 /*!< Number of log lines kept in RAM */
#define WT_LOG_ENTRY_LEN 200   /*!< Max characters per line (incl '\0') */

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief  Initialise the ring buffer mutex. Must be called before any
 *         task uses an APPLOG macro. Safe to call multiple times.
 */
void wt_log_init(void);

/**
 * @brief  Append a pre-formatted log line to the ring buffer.
 *         Called internally by the APPLOG macros; may also be used
 *         directly for injecting synthetic entries.
 * @param  line  Null-terminated string (truncated to WT_LOG_ENTRY_LEN-1).
 */
void wt_log_append(const char *line);

/**
 * @brief  Serialise new log entries (seq > from_seq) as a JSON object.
 *
 * The returned object has the form:
 * @code
 *   { "seq": <uint32>, "entries": [ "<line>", ... ] }
 * @endcode
 *
 * @param  from_seq   Sequence number of the last entry the caller has seen.
 *                    Pass 0 to receive all buffered entries.
 * @param  out_buf    Caller-allocated output buffer.
 * @param  buf_len    Size of out_buf in bytes.
 * @param  next_seq   [out] Value to pass as from_seq on the next call.
 * @return            Number of new entries written into out_buf.
 */
int wt_log_read_json(uint32_t from_seq, char *out_buf,
                     size_t buf_len, uint32_t *next_seq);

/* ------------------------------------------------------------------ */
/*  Logging macros                                                       */
/* ------------------------------------------------------------------ */

#define APPLOG(type, format, ...)                                         \
    do                                                                    \
    {                                                                     \
        struct timeval _tv;                                               \
        gettimeofday(&_tv, NULL);                                         \
        struct tm _t;                                                     \
        localtime_r(&_tv.tv_sec, &_t);                                    \
        int _ms = (int)(_tv.tv_usec / 1000);                              \
        char _line[WT_LOG_ENTRY_LEN];                                     \
        snprintf(_line, sizeof(_line),                                    \
                 "%02d-%02d-%04d %02d:%02d:%02d.%03d [" type "] " format, \
                 _t.tm_mday, _t.tm_mon + 1, _t.tm_year + 1900,            \
                 _t.tm_hour, _t.tm_min, _t.tm_sec, _ms,                   \
                 ##__VA_ARGS__);                                          \
        puts(_line);                                                      \
        fflush(stdout);                                                   \
        wt_log_append(_line);                                             \
    } while (0)

#define APPLOG_I(fmt, ...) APPLOG("INFO", fmt, ##__VA_ARGS__)
#define APPLOG_E(fmt, ...) APPLOG("ERROR", fmt, ##__VA_ARGS__)
#define APPLOG_W(fmt, ...) APPLOG("WARN", fmt, ##__VA_ARGS__)
#define APPLOG_D(fmt, ...) APPLOG("DEBUG", fmt, ##__VA_ARGS__)
#define APPLOG_V(fmt, ...) APPLOG("VERBOSE", fmt, ##__VA_ARGS__)

#endif /* WT_APP_LOG_H */
