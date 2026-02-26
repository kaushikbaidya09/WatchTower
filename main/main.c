#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_led.h"

void wt_task_main(void *pvParameters)
{
    while (1)
    {
        // APPLOG_I("wt_task_main running. Core ID: %d", xPortGetCoreID());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// Main application entry point
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE); // set all components Log level

    // Tasks on Core-00
    xTaskCreatePinnedToCore(wt_task_wifi, "WT_TASK_WIFI", 8096, NULL, 4, NULL, 0);
    
    // Tasks on Core-01
    xTaskCreatePinnedToCore(wt_task_main, "WT_TASK_MAIN", 4096, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(wt_task_led, "WT_TASK_LED", 16384, NULL, 5, NULL, 1);

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}