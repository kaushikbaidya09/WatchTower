#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#if IP_NAPT
#include "lwip/lwip_napt.h"
#endif
#include "lwip/err.h"
#include "lwip/sys.h"
#include "wt_app_wifi.h"
#include "wt_app_log.h"

#ifndef ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

/* STA Configuration */
#define WT_WIFI_STA_SSID "Kaushik's GT 2 Pro" //< Default wifi sta ssid
#define WT_WIFI_STA_PASSWD "24681355"         //< Default wifi sta ssid password
#define WT_WIFI_STA_RETRY 3                   //< Maximum retry on wifi station mode
#define WT_WIFI_STA_RETRY_WAIT_MS 10000       //< Retry wait period in milliseconds

/* AP Configuration */
#define WT_WIFI_AP_SSID "MyNetwork_KB"  //< Default wifi ap ssid
#define WT_WIFI_AP_PASSWD "password123" //< Default wifi ap ssid password
#define WT_WIFI_AP_CHANNEL 6            //< Wifi AP channel
#define WT_WIFI_AP_MAX_CONN 4           //< Maximum device limit

/* WIFI Events */
#define WT_WIFI_EVENT_CONNECT (BIT0)
#define WT_WIFI_EVENT_FAIL (BIT1)
#define WT_WIFI_EVENT_ALL (WT_WIFI_EVENT_CONNECT | WT_WIFI_EVENT_FAIL)

/* DHCP server option*/
#define WT_WIFI_DHCPS_OFFER_DNS 0x02

typedef struct
{
    const char *wifi_ssid;
    const char *wifi_pass;
} wifi_creds_t;

static wifi_creds_t wifi_creds[] = {
    {
        .wifi_ssid = "Hari 5th floor",
        .wifi_pass = "7259466152",
    },
    {
        .wifi_ssid = "KBs GT 2 Pro",
        .wifi_pass = "24681355",
    },
    {
        .wifi_ssid = "500 Server error!",
        .wifi_pass = "password_02",
    },
    {
        .wifi_ssid = "My Galaxy",
        .wifi_pass = "Galaxy@123",
    },
};
static uint8_t wc_index = 0;
static bool wifi_connected = false;
static int s_retry_num = 0;

static void next_wifi_creds(void)
{
    wc_index = (wc_index + 1) % (sizeof(wifi_creds) / sizeof(wifi_creds[0]));
}

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

/* FreeRTOS event group to signal when we are connected/disconnected */
static EventGroupHandle_t s_wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
        APPLOG_I("Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
        APPLOG_I("Station " MACSTR " left, AID=%d, reason:%d", MAC2STR(event->mac), event->aid, event->reason);
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
        APPLOG_I("Station started");
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // // Retry to reconnect
        // if (s_retry_num < WIFI_MAX_RETRY_PER_SSID)
        // {
        //     xEventGroupSetBits(s_wifi_event_group, WEVT_SET_CONFIG);
        // }
        // else
        // {
        //     xEventGroupSetBits(s_wifi_event_group, WEVT_CONNECTION_FAILED);
        // }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        APPLOG_I("Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WT_WIFI_EVENT_CONNECT);
    }
}

/* Initialize soft AP */
esp_netif_t *wifi_init_softap(void)
{
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = WT_WIFI_AP_SSID,
            .ssid_len = strlen(WT_WIFI_AP_SSID),
            .channel = WT_WIFI_AP_CHANNEL,
            .password = WT_WIFI_AP_PASSWD,
            .max_connection = WT_WIFI_AP_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(WT_WIFI_AP_PASSWD) == 0)
    {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    APPLOG_I("wifi_init_softap finished. SSID:%s password:%s channel:%d", WT_WIFI_AP_SSID, WT_WIFI_AP_PASSWD, WT_WIFI_AP_CHANNEL);

    return esp_netif_ap;
}

/* Initialize wifi station */
esp_netif_t *wifi_init_sta(void)
{
    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_sta_config = {
        .sta = {
            .ssid = WT_WIFI_STA_SSID,
            .password = WT_WIFI_STA_PASSWD,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .failure_retry_cnt = WT_WIFI_STA_RETRY,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

    APPLOG_I("wifi_init_sta finished.");

    return esp_netif_sta;
}

void softap_set_dns_addr(esp_netif_t *esp_netif_ap, esp_netif_t *esp_netif_sta)
{
    esp_netif_dns_info_t dns;
    esp_netif_get_dns_info(esp_netif_sta, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t dhcps_offer_option = WT_WIFI_DHCPS_OFFER_DNS;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(esp_netif_ap));
    ESP_ERROR_CHECK(esp_netif_dhcps_option(esp_netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_offer_option, sizeof(dhcps_offer_option)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(esp_netif_ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(esp_netif_ap));
}

void wt_task_wifi(void *pvParameters)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Initialize event group */
    s_wifi_event_group = xEventGroupCreate();

    /* Register Event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /*Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Initialize AP */
    APPLOG_I("ESP_WIFI_MODE_AP");
    esp_netif_t *esp_netif_ap = wifi_init_softap();

    /* Initialize STA */
    APPLOG_I("ESP_WIFI_MODE_STA");
    esp_netif_t *esp_netif_sta = wifi_init_sta();

    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_start());

    while (1)
    {
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WT_WIFI_EVENT_ALL, pdTRUE, pdFALSE, portMAX_DELAY);

        /* xEventGroupWaitBits() returns the bits before the call returned,
         * hence we can test which event actually happened. */
        if (bits & WT_WIFI_EVENT_CONNECT)
        {
            APPLOG_I("connected to ap SSID:%s password:%s", WT_WIFI_STA_SSID, WT_WIFI_STA_PASSWD);
            softap_set_dns_addr(esp_netif_ap, esp_netif_sta);

            obtain_time();
        }
        else if (bits & WT_WIFI_EVENT_FAIL)
        {
            APPLOG_I("Failed to connect to SSID:%s, password:%s", WT_WIFI_STA_SSID, WT_WIFI_STA_PASSWD);
        }
        else
        {
            APPLOG_I("UNEXPECTED EVENT");
            return;
        }
    }

    /* Set sta as the default interface */
    esp_netif_set_default_netif(esp_netif_sta);

    /* Enable napt on the AP netif */
    if (esp_netif_napt_enable(esp_netif_ap) != ESP_OK)
    {
        APPLOG_I("NAPT not enabled on the netif: %p", esp_netif_ap);
    }
}
