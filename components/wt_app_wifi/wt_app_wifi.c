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
#define WT_WIFI_STA_RETRY 2                   //< Maximum retry on wifi station mode
#define WT_WIFI_STA_RETRY_WAIT_MS 10000       //< Retry wait period in milliseconds

/* AP Configuration */
#define WT_WIFI_AP_SSID "MyNetwork_KB"  //< Default wifi ap ssid
#define WT_WIFI_AP_PASSWD "password123" //< Default wifi ap ssid password
#define WT_WIFI_AP_CHANNEL 6            //< Wifi AP channel
#define WT_WIFI_AP_MAX_CONN 4           //< Maximum device limit

/* DHCP server option*/
#define WT_WIFI_DHCPS_OFFER_DNS 0x02

typedef struct
{
    const char ssid[64];
    const char passwd[64];
} wifi_creds_t;

static wifi_creds_t wt_wifi_sta_profile[] = {
    {
        .ssid = "Hari 5th floor",
        .passwd = "7259466152",
    },
    {
        .ssid = "Kaushik's GT 2 Pro",
        .passwd = "24681355",
    },
};
static uint8_t wt_wifi_sta_active_profile = 0;
static bool wt_wifi_sta_started = false;
static bool wt_wifi_sta_is_connected = false;
static uint8_t wt_wifi_sta_retry_count = WT_WIFI_STA_RETRY;
static wifi_config_t wifi_ap_config;
static wifi_config_t wifi_sta_config;
static esp_netif_t *esp_netif_ap = NULL;
static esp_netif_t *esp_netif_sta = NULL;

static void wt_sta_connect_next_profile(void)
{
    wt_wifi_sta_active_profile = (wt_wifi_sta_active_profile + 1) % (sizeof(wt_wifi_sta_profile) / sizeof(wt_wifi_sta_profile[0]));

    strlcpy((char *)wifi_sta_config.sta.ssid, wt_wifi_sta_profile[wt_wifi_sta_active_profile].ssid, sizeof(wifi_sta_config.sta.ssid));
    strlcpy((char *)wifi_sta_config.sta.password, wt_wifi_sta_profile[wt_wifi_sta_active_profile].passwd, sizeof(wifi_sta_config.sta.password));
    wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_sta_config.sta.failure_retry_cnt = WT_WIFI_STA_RETRY;
    wifi_sta_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_scan_stop());
    APPLOG_I("Next profile '%s', '%s'", wifi_sta_config.sta.ssid, wifi_sta_config.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));
    esp_wifi_connect();
}

static void obtain_time(void)
{
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.windows.com");
    esp_sntp_init();

    // Wait until time is set
    int retry = 0;
    time_t now = 0;
    struct tm timeinfo = {0};
    const int retry_count = 10;
    while (timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    // Configure IST (UTC+5:30)
    setenv("TZ", "IST-5:30", 1);
    tzset();

    if (retry != retry_count)
        APPLOG_I("Time synchronized: %s", asctime(&timeinfo));
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

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_WIFI_READY:
            APPLOG_I("EVENT >> WIFI_EVENT_WIFI_READY");
            break;
        case WIFI_EVENT_SCAN_DONE:
            APPLOG_I("EVENT >> WIFI_EVENT_SCAN_DONE");
            break;

            // Access Point (AP)
        case WIFI_EVENT_AP_START:
            APPLOG_I("EVENT >> WIFI_EVENT_AP_START");
            break;
        case WIFI_EVENT_AP_STOP:
            APPLOG_I("EVENT >> WIFI_EVENT_AP_STOP");
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            APPLOG_I("EVENT >> WIFI_EVENT_AP_STACONNECTED");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            APPLOG_I("EVENT >> WIFI_EVENT_AP_STADISCONNECTED");
            break;

            // Station (STA)
        case WIFI_EVENT_STA_START:
            APPLOG_I("EVENT >> WIFI_EVENT_STA_START");
            wt_wifi_sta_started = true;
            break;
        case WIFI_EVENT_STA_STOP:
            APPLOG_I("EVENT >> WIFI_EVENT_STA_STOP");
            wt_wifi_sta_started = false;
            break;
        case WIFI_EVENT_STA_CONNECTED:
            APPLOG_I("EVENT >> WIFI_EVENT_STA_CONNECTED");
            wt_wifi_sta_is_connected = true;
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            APPLOG_I("EVENT >> WIFI_EVENT_STA_DISCONNECTED");
            wt_wifi_sta_is_connected = false;
            break;

        default:
            break;
        }
    }
    else if (event_base == IP_EVENT)
    {
        switch (event_id)
        {
        case IP_EVENT_STA_GOT_IP:
            softap_set_dns_addr(esp_netif_ap, esp_netif_sta);
            obtain_time();
            break;
        case IP_EVENT_STA_LOST_IP:
            break;
        default:
            break;
        }
    }
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

    /* Register Event handler */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    /* Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* WiFi Station Config */
    strlcpy((char *)wifi_sta_config.sta.ssid, wt_wifi_sta_profile[wt_wifi_sta_active_profile].ssid, sizeof(wifi_sta_config.sta.ssid));
    strlcpy((char *)wifi_sta_config.sta.password, wt_wifi_sta_profile[wt_wifi_sta_active_profile].passwd, sizeof(wifi_sta_config.sta.password));
    wifi_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_sta_config.sta.failure_retry_cnt = WT_WIFI_STA_RETRY;
    wifi_sta_config.sta.threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    wifi_sta_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    /* WiFi Access Point Config */
    strlcpy((char *)wifi_ap_config.ap.ssid, WT_WIFI_AP_SSID, sizeof(wifi_ap_config.ap.ssid));
    strlcpy((char *)wifi_ap_config.ap.password, WT_WIFI_AP_PASSWD, sizeof(wifi_ap_config.ap.password));
    wifi_ap_config.ap.ssid_len = strlen(WT_WIFI_AP_SSID);
    wifi_ap_config.ap.channel = WT_WIFI_AP_CHANNEL;
    wifi_ap_config.ap.max_connection = WT_WIFI_AP_MAX_CONN;
    wifi_ap_config.ap.authmode = strlen(WT_WIFI_AP_PASSWD) != 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_ap_config.ap.pmf_cfg.required = false;

    /* Initialize AP */
    APPLOG_I("ESP_WIFI_MODE_AP");
    esp_netif_ap = esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    /* Initialize STA */
    APPLOG_I("ESP_WIFI_MODE_STA");
    esp_netif_sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config));

    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_start());

    vTaskDelay(pdMS_TO_TICKS(10000));

    while (1)
    {
        // Retry connection if not connected
        if ((wt_wifi_sta_started == true) && (wt_wifi_sta_is_connected == false))
        {
            if (wt_wifi_sta_retry_count-- > 0)
            {
                esp_wifi_connect();
                APPLOG_I("WiFi Connect (%d/%d)", (WT_WIFI_STA_RETRY - wt_wifi_sta_retry_count), WT_WIFI_STA_RETRY);
            }
            else
            {
                esp_wifi_disconnect();
                wt_wifi_sta_retry_count = WT_WIFI_STA_RETRY;
                wt_sta_connect_next_profile();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WT_WIFI_STA_RETRY_WAIT_MS));
    }

    /* Set sta as the default interface */
    esp_netif_set_default_netif(esp_netif_sta);

    /* Enable napt on the AP netif */
    if (esp_netif_napt_enable(esp_netif_ap) != ESP_OK)
    {
        APPLOG_I("NAPT not enabled on the netif: %p", esp_netif_ap);
    }
}
