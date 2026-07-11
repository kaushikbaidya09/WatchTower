#ifndef WT_APP_TIME_H
#define WT_APP_TIME_H

#include <stdbool.h>
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

/**
 * @brief  Read the current RTC time.
 * @return true if the read succeeded and *time was updated; false on I2C
 *         failure or invalid arguments, in which case *time is left untouched.
 */
bool wt_time_get_time(wt_time_t *time);
void wt_time_set_time(const wt_time_t *time);

#endif // WT_APP_TIME_H