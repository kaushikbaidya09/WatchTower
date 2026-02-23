#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_led.h"

static void obtain_time(void)
{
    // Configure before init
    // esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Wait until time is set
    time_t now = 0;
    struct tm timeinfo = {0};

    int retry = 0;
    const int retry_count = 10;
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
    {
        APPLOG_I("Getting time.....!");
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    // Configure IST (UTC+5:30)
    setenv("TZ", "IST-5:30", 1);
    tzset();

    if (retry == retry_count)
    {
        APPLOG_I("Failed to get time from NTP server");
    }
    else
    {
        APPLOG_I("Time synchronized: %s", asctime(&timeinfo));
    }
}

void wt_task_main(void *pvParameters)
{
    while (1)
    {
        APPLOG_I("wt_task_main running. Core ID: %d", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Main application entry point
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO); // set all components Log level

    xTaskCreatePinnedToCore(wt_task_main, "WT_TASK_MAIN", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(wt_task_wifi, "WT_TASK_WIFI", 4096, NULL, 4, NULL, 0);
    xTaskCreatePinnedToCore(wt_task_led, "WT_TASK_LED", 8192, NULL, 5, NULL, 1);

    // vTaskDelay(pdMS_TO_TICKS(10000));
    // obtain_time();

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}