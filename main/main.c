#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_log.h"

void generate_data_task(void *pvParameter)
{
    int count = 0;
    while (1)
    {
        APPLOG_I("generate_data_task! count: %d", count++);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

void app_main()
{
    // LOG SETUP
    esp_log_level_set("*", ESP_LOG_MAX); // set all components Log level

    xTaskCreate(&generate_data_task, "generate_data_task", 2048, NULL, 5, NULL);
    APPLOG_I("Generate data task created!.");

    int count = 0;
    while (1)
    {
        APPLOG_I("Default task! count: %d", count++);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}