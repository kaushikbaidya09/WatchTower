#ifndef WT_APP_TIME_H
#define WT_APP_TIME_H

#include "driver/i2c_master.h"
#include "esp_err.h"

#define WT_RTC_I2C_ADDR 0x68

typedef struct {
    int sec;
    int min;
    int hour;
    int day;
    int month;
    int year;
} wt_time_t;

void wt_time_init(void);
void wt_time_get_time(wt_time_t *time);
void wt_time_set_time(const wt_time_t *time);

#endif // WT_APP_TIME_H