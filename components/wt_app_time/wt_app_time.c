//=============================================================================
/*!
    \file    wt_app_time.c
    \brief   DS3231 real-time clock over I2C.
*/
//=============================================================================

// Reads and writes the RTC, and sets the system clock from it at boot.
// NTP sync is in wt_app_wifi.c.

#include "wt_app_time.h"
#include "wt_app_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <sys/time.h>
#include <time.h>

#define WT_RTC_I2C_PORT I2C_NUM_0
#define WT_RTC_SCL_IO   12
#define WT_RTC_SDA_IO   13
#define WT_RTC_FREQ_HZ  100000

#define DS3231_REG_TIME 0x00
#define RTC_RETRY_COUNT 3

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_rtc_dev = NULL;
static SemaphoreHandle_t s_rtc_mutex     = NULL;

/*!
    \brief  Convert a BCD register value to a number.
*/
static int bcd_to_dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

/*!
    \brief  Convert a number to a BCD register value.
*/
static uint8_t dec_to_bcd(int val)
{
    return ((val / 10) << 4) | (val % 10);
}

/*!
    \brief  Check that every field of a time is in range.
*/
static bool wt_rtc_validate_time(const wt_time_t *t)
{
    if (!t)
        return false;

    if (t->sec < 0 || t->sec > 59 || t->min < 0 || t->min > 59 || t->hour < 0 || t->hour > 23)
        return false;

    if (t->day < 1 || t->day > 31)
        return false;

    if (t->month < 1 || t->month > 12)
        return false;

    if (t->year < 2000 || t->year > 2099)
        return false;

    return true;
}

/*!
    \brief  Read the time from the RTC.
    \param[out]  time  Receives the time; unchanged if the read fails.
    \return true if a valid time was read.
*/
bool wt_time_get_time(wt_time_t *time)
{
    if (time == NULL || s_rtc_dev == NULL)
    {
        return false;
    }

    if (!s_rtc_mutex || xSemaphoreTake(s_rtc_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        wt_log_error("Unable to get s_rtc_mutex");
        return false;
    }

    uint8_t reg = DS3231_REG_TIME;
    uint8_t data[7];
    esp_err_t ret = ESP_FAIL;

    for (int i = 0; i < RTC_RETRY_COUNT; i++)
    {
        ret = i2c_master_transmit_receive(s_rtc_dev, &reg, 1, data, 7, pdMS_TO_TICKS(1000));

        if (ret == ESP_OK)
            break;
    }

    bool ok = false;
    if (ret == ESP_OK)
    {
        wt_time_t read = {
            .sec   = bcd_to_dec(data[0] & 0x7F),
            .min   = bcd_to_dec(data[1] & 0x7F),
            .hour  = bcd_to_dec(data[2] & 0x3F),
            .day   = bcd_to_dec(data[4] & 0x3F),
            .month = bcd_to_dec(data[5] & 0x1F),
            .year  = 2000 + bcd_to_dec(data[6]),
        };

        // Reject garbage read from a faulty bus or a different chip.
        if (wt_rtc_validate_time(&read))
        {
            *time = read;
            ok    = true;
        }
        else
        {
            wt_log_error("RTC returned an invalid time");
        }
    }
    else
    {
        wt_log_error("RTC read failed");
    }

    xSemaphoreGive(s_rtc_mutex);
    return ok;
}

/*!
    \brief  Set the RTC. An invalid time is ignored.
    \param[in]  time  Time to set, in UTC.
*/
void wt_time_set_time(const wt_time_t *time)
{
    if (!wt_rtc_validate_time(time) || s_rtc_dev == NULL)
    {
        return;
    }

    if (!s_rtc_mutex || xSemaphoreTake(s_rtc_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
    {
        wt_log_error("Unable to get s_rtc_mutex");
        return;
    }

    uint8_t data[8];

    data[0] = DS3231_REG_TIME;
    data[1] = dec_to_bcd(time->sec) & 0x7F;
    data[2] = dec_to_bcd(time->min);
    data[3] = dec_to_bcd(time->hour);
    data[4] = 1;
    data[5] = dec_to_bcd(time->day);
    data[6] = dec_to_bcd(time->month);
    data[7] = dec_to_bcd(time->year % 100);

    esp_err_t ret = ESP_FAIL;

    for (int i = 0; i < RTC_RETRY_COUNT; i++)
    {
        ret = i2c_master_transmit(s_rtc_dev, data, sizeof(data), pdMS_TO_TICKS(1000));

        if (ret == ESP_OK)
        {
            break;
        }
    }

    if (ret == ESP_OK)
    {
        wt_log_info("Time set successfully");
    }
    else
    {
        wt_log_error("Failed to set time");
    }

    xSemaphoreGive(s_rtc_mutex);

    wt_log_info("RTC Time: %02d/%02d/%04d %02d:%02d:%02d", time->day, time->month, time->year,
                time->hour, time->min, time->sec);

    return;
}

/*!
    \brief  Convert a UTC date and time to seconds since 1970.
*/
static int64_t utc_to_epoch(const wt_time_t *t)
{
    int year  = t->year;
    int month = t->month;
    if (month <= 2)
    {
        year -= 1;
    }
    int era         = year / 400;
    int year_of_era = year - era * 400;
    int day_of_year = (153 * (month + ((month > 2) ? -3 : 9)) + 2) / 5 + t->day - 1;
    int day_of_era  = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    int64_t days    = (int64_t)era * 146097 + day_of_era - 719468;

    return days * 86400 + (int64_t)t->hour * 3600 + (int64_t)t->min * 60 + t->sec;
}

/*!
    \brief  Set up the I2C bus and the RTC, then set the system clock from it.
*/
void wt_time_init(void)
{
    if (s_rtc_dev == NULL)
    {
        i2c_master_bus_config_t bus_config = {
            .clk_source                   = I2C_CLK_SRC_DEFAULT,
            .i2c_port                     = WT_RTC_I2C_PORT,
            .scl_io_num                   = WT_RTC_SCL_IO,
            .sda_io_num                   = WT_RTC_SDA_IO,
            .glitch_ignore_cnt            = 7,
            .flags.enable_internal_pullup = true,
        };
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = WT_RTC_I2C_ADDR,
            .scl_speed_hz    = WT_RTC_FREQ_HZ,
        };

        s_rtc_mutex = xSemaphoreCreateMutex();
        if (s_rtc_mutex == NULL || i2c_new_master_bus(&bus_config, &s_i2c_bus) != ESP_OK ||
            i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_rtc_dev) != ESP_OK)
        {
            wt_log_error("RTC init failed (SCL=%d SDA=%d)", WT_RTC_SCL_IO, WT_RTC_SDA_IO);
            return;
        }
        wt_log_info("RTC initialized (SCL=%d SDA=%d)", WT_RTC_SCL_IO, WT_RTC_SDA_IO);
    }

    wt_time_t rtc_time;
    if (!wt_time_get_time(&rtc_time))
    {
        wt_log_warn("RTC read failed system time left unset");
        return;
    }

    // The RTC keeps UTC. The epoch is calculated directly instead of
    // switching TZ around mktime(), which cannot be undone reliably.
    struct timeval tv = {.tv_sec = (time_t)utc_to_epoch(&rtc_time), .tv_usec = 0};
    settimeofday(&tv, NULL);

    wt_log_info("System time set from RTC: %02d/%02d/%04d %02d:%02d:%02d UTC", rtc_time.day,
                rtc_time.month, rtc_time.year, rtc_time.hour, rtc_time.min, rtc_time.sec);
}
