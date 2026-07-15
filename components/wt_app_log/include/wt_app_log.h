/*!
    \file   wt_app_log.h
    \brief  Application logging console output + in-memory ring buffer.

    \details
    Every wt_log_x() call prints the formatted line to stdout and appends
    it to an internal ring buffer. wt_app_web.c reads new entries out via
    wt_log_read_json() and embeds them in the "logs" field of the periodic
    WebSocket "full" frame (there is no separate HTTP endpoint for this).
    Call wt_log_init() once before any task that uses wt_log_x().
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

#define WT_LOG_BUF_ENTRIES 128 ///< Number of log lines kept in RAM
#define WT_LOG_ENTRY_LEN 200   ///< Max characters per line (incl '\0')

void wt_log_init(void);

void wt_log_append(const char *line);

int wt_log_read_json(uint32_t from_seq, char *out_buf,
                     size_t buf_len, uint32_t *next_seq);

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

#define wt_log_info(fmt, ...) APPLOG("INFO", fmt, ##__VA_ARGS__)
#define wt_log_error(fmt, ...) APPLOG("ERROR", fmt, ##__VA_ARGS__)
#define wt_log_warn(fmt, ...) APPLOG("WARN", fmt, ##__VA_ARGS__)
#define wt_log_debug(fmt, ...) APPLOG("DEBUG", fmt, ##__VA_ARGS__)
#define wt_log_verbose(fmt, ...) APPLOG("VERBOSE", fmt, ##__VA_ARGS__)

#endif /* WT_APP_LOG_H */
