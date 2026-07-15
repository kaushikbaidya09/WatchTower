/*!
    \file   wt_app_time.c
    \brief  DS3231 RTC driver over I2C.

    \details
    Reads/writes the RTC and syncs the system clock from it at boot
    (RTC -> system time). NTP sync itself lives in wt_app_wifi.c.
 */

#include "wt_app_time.h"
#include "wt_app_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include <time.h>

#define WT_RTC_I2C_PORT I2C_NUM_0
#define WT_RTC_SCL_IO 12
#define WT_RTC_SDA_IO 13
#define WT_RTC_FREQ_HZ 100000
#define WT_RTC_I2C_ADDR 0x68

#define DS3231_REG_TIME 0x00
#define RTC_RETRY_COUNT 3

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_rtc_dev = NULL;
static SemaphoreHandle_t s_rtc_mutex = NULL;

/*!
    \brief  Convert a DS3231 BCD register byte to decimal.
 */
static int bcd_to_dec(uint8_t val)
{
    return ((val >> 4) * 10) + (val & 0x0F);
}

/*!
    \brief  Convert a decimal value to a DS3231 BCD register byte.
 */
static uint8_t dec_to_bcd(int val)
{
    return ((val / 10) << 4) | (val % 10);
}

/*!
    \brief  Initialize the I2C master bus used to communicate with the DS3231 RTC.
 */
static esp_err_t wt_rtc_i2c_init(void)
{
    if (s_i2c_bus != NULL)
        return ESP_OK;

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = WT_RTC_I2C_PORT,
        .scl_io_num = WT_RTC_SCL_IO,
        .sda_io_num = WT_RTC_SDA_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_config, &s_i2c_bus);

    if (ret == ESP_OK)
        wt_log_info("I2C initialized (SCL=%d SDA=%d)", WT_RTC_SCL_IO, WT_RTC_SDA_IO);
    else
        wt_log_error("I2C init failed");

    return ret;
}

/*!
    \brief  Initialize the RTC device: brings up the I2C bus, creates the RTC
            mutex if needed, and registers the DS3231 device on the I2C bus.
 */
void wt_rtc_init(void)
{
    esp_err_t err = wt_rtc_i2c_init();
    if (err != ESP_OK)
    {
        wt_log_error("Failed to initialize I2C");
        return;
    }

    if (s_rtc_mutex == NULL)
    {
        s_rtc_mutex = xSemaphoreCreateMutex();
        if (s_rtc_mutex == NULL)
        {
            wt_log_error("Mutex creation failed");
            return;
        }
    }

    if (s_rtc_dev != NULL)
    {
        return;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = WT_RTC_I2C_ADDR,
        .scl_speed_hz = WT_RTC_FREQ_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_rtc_dev);

    if (ret == ESP_OK)
    {
        wt_log_info("RTC initialized");
    }
    else
    {
        wt_log_error("RTC init failed");
    }

    return;
}

/*!
    \brief  Validate that a wt_time_t value's fields are within valid ranges
            for the DS3231 RTC.
 */
static bool wt_rtc_validate_time(const wt_time_t *t)
{
    if (!t)
        return false;

    if (t->sec > 59 || t->min > 59 || t->hour > 23)
        return false;

    if (t->day == 0 || t->day > 31)
        return false;

    if (t->month == 0 || t->month > 12)
        return false;

    if (t->year < 2000 || t->year > 2099)
        return false;

    return true;
}

/*!
    \brief  Read the current RTC time.
    \param[out]  time  Destination for the time read from the RTC; left
                        untouched if the read fails.
    \return true if the read succeeded and *time was updated; false on I2C
            failure or invalid arguments, in which case *time is left untouched.
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
        ret = i2c_master_transmit_receive(
            s_rtc_dev,
            &reg, 1,
            data, 7,
            pdMS_TO_TICKS(1000));

        if (ret == ESP_OK)
            break;
    }

    if (ret == ESP_OK)
    {
        time->sec = bcd_to_dec(data[0] & 0x7F);
        time->min = bcd_to_dec(data[1]);
        time->hour = bcd_to_dec(data[2] & 0x3F);
        time->day = bcd_to_dec(data[4]);
        time->month = bcd_to_dec(data[5] & 0x1F);
        time->year = 2000 + bcd_to_dec(data[6]);

        wt_log_info("RTC Time: %02d/%02d/%04d %02d:%02d:%02d",
                 time->day, time->month, time->year,
                 time->hour, time->min, time->sec);
    }
    else
    {
        wt_log_error("RTC read failed");
    }

    xSemaphoreGive(s_rtc_mutex);
    return (ret == ESP_OK);
}

/*!
    \brief  Write a time value to the RTC, after validating it.
    \param[in]  time  Time value to program into the RTC.
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
        ret = i2c_master_transmit(
            s_rtc_dev,
            data,
            sizeof(data),
            pdMS_TO_TICKS(1000));

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

    wt_log_info("RTC Time: %02d/%02d/%04d %02d:%02d:%02d",
             time->day, time->month, time->year,
             time->hour, time->min, time->sec);

    return;
}

/*!
    \brief  Read the RTC and synchronize the system clock from it via
            settimeofday(), temporarily switching to TZ=UTC0 for the
            RTC (UTC) -> epoch conversion and restoring the prior TZ after.
 */
static void wt_time_set_system(void)
{
    wt_time_t rtc_time;
    if (!wt_time_get_time(&rtc_time))
    {
        wt_log_warn("RTC read failed system time left unset");
        return;
    }

    struct tm t = {0};
    t.tm_sec = rtc_time.sec;
    t.tm_min = rtc_time.min;
    t.tm_hour = rtc_time.hour;
    t.tm_mday = rtc_time.day;
    t.tm_mon = rtc_time.month - 1;
    t.tm_year = rtc_time.year - 1900;

    // Convert UTC tm -> epoch safely
    char *old_tz = getenv("TZ");

    setenv("TZ", "UTC0", 1);
    tzset();

    time_t now = mktime(&t);

    // Restore original TZ
    if (old_tz)
        setenv("TZ", old_tz, 1);
    else
        unsetenv("TZ");

    tzset();

    struct timeval tv = {
        .tv_sec = now,
        .tv_usec = 0};

    settimeofday(&tv, NULL);

    wt_log_info("System time updated from RTC (UTC)");

    wt_log_info("RTC Time: %02d/%02d/%04d %02d:%02d:%02d",
             rtc_time.day, rtc_time.month, rtc_time.year,
             rtc_time.hour, rtc_time.min, rtc_time.sec);
}

/*!
    \brief  Initialize the RTC hardware and synchronize the system clock
            from it at boot.
 */
void wt_time_init(void)
{
    wt_rtc_init();
    wt_time_set_system();
}