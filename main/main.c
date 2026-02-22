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
#include "app_log.h"
#include "wifi.h"
#include "led.h"

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

void generate_data_task(void *pvParameter)
{
    int count = 0;
    while (1)
    {
        APPLOG_I("generate_data_task! count: %d", count++);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Task function for Core 0
void task_core_0(void *pvParameters)
{
    wifi_app_main();

    while (1)
    {
        // APPLOG_I("Task running on Core 0. Core ID: %d", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// Task function for Core 1
void task_core_1(void *pvParameters)
{
    while (1)
    {
        // APPLOG_I("Task running on Core 1. Core ID: %d", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Main application entry point
void app_main(void)
{
    // LOG SETUP
    esp_log_level_set("*", ESP_LOG_INFO); // set all components Log level

    // Create Task 1 and pin it to Core 0
    xTaskCreatePinnedToCore(
        task_core_0, /* Function to implement the task */
        "TaskCore0", /* Name of the task */
        4096,        /* Stack size in words */
        NULL,        /* Task input parameter */
        4,           /* Priority of the task */
        NULL,        /* Task handle */
        0);          /* Core where the task should run (0 or 1) */

    // Create Task 2 and pin it to Core 1
    xTaskCreatePinnedToCore(
        task_core_1, /* Function to implement the task */
        "TaskCore1", /* Name of the task */
        4096,        /* Stack size in words */
        NULL,        /* Task input parameter */
        5,           /* Priority of the task */
        NULL,        /* Task handle */
        1);          /* Core where the task should run (0 or 1) */

    xTaskCreate(&led_task_main, "led_task", 8192, NULL, 5, NULL);
    APPLOG_I("LED task created!.");

    vTaskDelay(pdMS_TO_TICKS(10000));
    obtain_time();

    xTaskCreate(&generate_data_task, "generate_data_task", 4096, NULL, 5, NULL);
    APPLOG_I("Generate data task created!.");
}