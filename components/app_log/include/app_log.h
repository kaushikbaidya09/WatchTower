#ifndef APP_LOG_H
#define APP_LOG_H

#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "esp_log.h"

#define APPLOG(type, format, ...)                         \
    do                                                    \
    {                                                     \
        struct timeval tv;                                \
        gettimeofday(&tv, NULL);                          \
        struct tm t;                                      \
        localtime_r(&tv.tv_sec, &t);                      \
        int millis = tv.tv_usec / 1000;                   \
        printf("%02d-%02d-%04d_%02d:%02d:%02d.%03d_%s:%d" \
               "_[" type "] " format "\n",                \
               t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, \
               t.tm_hour, t.tm_min, t.tm_sec, millis,     \
               __FILE__, __LINE__, ##__VA_ARGS__);        \
        fflush(stdout);                                   \
    } while (0)

#define APPLOG_I(format, ...) APPLOG("INFO", format, ##__VA_ARGS__)
#define APPLOG_E(format, ...) APPLOG("ERROR", format, ##__VA_ARGS__)
#define APPLOG_W(format, ...) APPLOG("WARN", format, ##__VA_ARGS__)
#define APPLOG_D(format, ...) APPLOG("DEBUG", format, ##__VA_ARGS__)
#define APPLOG_V(format, ...) APPLOG("VERBOSE", format, ##__VA_ARGS__)

#endif // APP_LOG_H