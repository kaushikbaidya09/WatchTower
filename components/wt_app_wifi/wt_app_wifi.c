/*!
    \file   wt_app_wifi.c
    \brief  WiFi AP+STA driver with dynamic NVS-backed profile management.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"
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

/* ------------------------------------------------------------------ */
/*  AP configuration (static)                                           */
/* ------------------------------------------------------------------ */
#define WT_WIFI_AP_SSID "WatchTower"
#define WT_WIFI_AP_PASSWD "watch@360"
#define WT_WIFI_AP_CHANNEL 6
#define WT_WIFI_AP_MAX_CONN 4
#define WT_WIFI_AP_IP "192.168.4.1"

/* ------------------------------------------------------------------ */
/*  STA retry policy                                                     */
/* ------------------------------------------------------------------ */
#define WT_WIFI_STA_RETRY 2
#define WT_WIFI_STA_RETRY_WAIT_MS 10000

/* ------------------------------------------------------------------ */
/*  NVS keys                                                             */
/* ------------------------------------------------------------------ */
#define WT_NVS_NS "wt_wifi"
#define WT_NVS_KEY_CNT "prof_cnt"
#define WT_NVS_KEY_SSID "ssid_%d"
#define WT_NVS_KEY_PASS "pass_%d"

/* DHCP server DNS option */
#define WT_WIFI_DHCPS_OFFER_DNS 0x02

/* ------------------------------------------------------------------ */
/*  Module state                                                         */
/* ------------------------------------------------------------------ */
static SemaphoreHandle_t s_mutex = NULL;
static wt_wifi_profile_t s_profiles[WT_WIFI_MAX_PROFILES];
static int s_profile_cnt = 0;
static int s_active_profile = 0;

static wt_wifi_status_t s_status; /*!< Protected by s_mutex */

static wifi_config_t s_ap_cfg;
static wifi_config_t s_sta_cfg;
static esp_netif_t *s_netif_ap = NULL;
static esp_netif_t *s_netif_sta = NULL;

static int s_retry_count = WT_WIFI_STA_RETRY;

/* ------------------------------------------------------------------ */
/*  NVS helpers                                                          */
/* ------------------------------------------------------------------ */

static void nvs_save_profiles(void)
{
    nvs_handle_t h;
    if (nvs_open(WT_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;

    nvs_set_i32(h, WT_NVS_KEY_CNT, s_profile_cnt);

    char key[20];
    for (int i = 0; i < s_profile_cnt; i++)
    {
        snprintf(key, sizeof(key), WT_NVS_KEY_SSID, i);
        nvs_set_str(h, key, s_profiles[i].ssid);
        snprintf(key, sizeof(key), WT_NVS_KEY_PASS, i);
        nvs_set_str(h, key, s_profiles[i].passwd);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_load_profiles(void)
{
    nvs_handle_t h;
    if (nvs_open(WT_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        /* No stored profiles — seed with defaults */
        s_profile_cnt = 3;
        strlcpy(s_profiles[0].ssid, "Wokwi-GUEST", WT_WIFI_SSID_LEN);
        strlcpy(s_profiles[0].passwd, "", WT_WIFI_PASS_LEN);
        strlcpy(s_profiles[1].ssid, "Kaushik's GT 2 Pro", WT_WIFI_SSID_LEN);
        strlcpy(s_profiles[1].passwd, "24681355", WT_WIFI_PASS_LEN);
        strlcpy(s_profiles[2].ssid, "Hari 5th floor", WT_WIFI_SSID_LEN);
        strlcpy(s_profiles[2].passwd, "7259466152", WT_WIFI_PASS_LEN);
        nvs_save_profiles();
        return;
    }

    int32_t cnt = 0;
    nvs_get_i32(h, WT_NVS_KEY_CNT, &cnt);
    s_profile_cnt = (cnt > WT_WIFI_MAX_PROFILES) ? WT_WIFI_MAX_PROFILES : (int)cnt;

    char key[20];
    size_t len;
    for (int i = 0; i < s_profile_cnt; i++)
    {
        len = WT_WIFI_SSID_LEN;
        snprintf(key, sizeof(key), WT_NVS_KEY_SSID, i);
        nvs_get_str(h, key, s_profiles[i].ssid, &len);

        len = WT_WIFI_PASS_LEN;
        snprintf(key, sizeof(key), WT_NVS_KEY_PASS, i);
        nvs_get_str(h, key, s_profiles[i].passwd, &len);
    }
    nvs_close(h);
}

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void apply_sta_profile(int index)
{
    memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
    strlcpy((char *)s_sta_cfg.sta.ssid,
            s_profiles[index].ssid, sizeof(s_sta_cfg.sta.ssid));
    strlcpy((char *)s_sta_cfg.sta.password,
            s_profiles[index].passwd, sizeof(s_sta_cfg.sta.password));
    s_sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    s_sta_cfg.sta.failure_retry_cnt = WT_WIFI_STA_RETRY;
    s_sta_cfg.sta.threshold.authmode =
        (strlen(s_profiles[index].passwd) == 0)
            ? WIFI_AUTH_OPEN
            : ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    s_sta_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
    s_active_profile = index;
}

static void softap_set_dns(void)
{
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK)
        return;
    uint8_t opt = WT_WIFI_DHCPS_OFFER_DNS;
    esp_netif_dhcps_stop(s_netif_ap);
    esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER, &opt, sizeof(opt));
    esp_netif_set_dns_info(s_netif_ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(s_netif_ap);
}

static void obtain_time(void)
{
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "time.windows.com");
    esp_sntp_init();

    time_t now = 0;
    struct tm ti = {0};
    int retry = 0;
    while (ti.tm_year < (2016 - 1900) && ++retry < 15)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &ti);
    }
    setenv("TZ", "IST-5:30", 1);
    tzset();
    if (retry < 15)
        APPLOG_I("NTP synced: %s", asctime(&ti));
    else
        APPLOG_W("NTP sync timed out");
}

/* ------------------------------------------------------------------ */
/*  Event handler                                                        */
/* ------------------------------------------------------------------ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            APPLOG_I("STA started");
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.sta_started = true;
            xSemaphoreGive(s_mutex);
            break;

        case WIFI_EVENT_STA_STOP:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.sta_started = false;
            s_status.sta_connected = false;
            xSemaphoreGive(s_mutex);
            break;

        case WIFI_EVENT_STA_CONNECTED:
        {
            wifi_event_sta_connected_t *evt = (wifi_event_sta_connected_t *)data;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.sta_connected = true;
            s_status.sta_active_profile = (uint8_t)s_active_profile;
            snprintf(s_status.sta_ssid, WT_WIFI_SSID_LEN,
                     "%.*s", evt->ssid_len, evt->ssid);
            xSemaphoreGive(s_mutex);
            APPLOG_I("STA connected: %.*s", evt->ssid_len, evt->ssid);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.sta_connected = false;
            memset(s_status.sta_ip, 0, sizeof(s_status.sta_ip));
            xSemaphoreGive(s_mutex);
            APPLOG_W("STA disconnected");
            break;

        case WIFI_EVENT_AP_START:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.ap_active = true;
            strlcpy(s_status.ap_ssid, WT_WIFI_AP_SSID, WT_WIFI_SSID_LEN);
            strlcpy(s_status.ap_ip, WT_WIFI_AP_IP, sizeof(s_status.ap_ip));
            xSemaphoreGive(s_mutex);
            APPLOG_I("AP started: %s", WT_WIFI_AP_SSID);
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.ap_clients++;
            xSemaphoreGive(s_mutex);
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_status.ap_clients > 0)
                s_status.ap_clients--;
            xSemaphoreGive(s_mutex);
            break;

        default:
            break;
        }
    }
    else if (base == IP_EVENT)
    {
        if (id == IP_EVENT_STA_GOT_IP)
        {
            ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            snprintf(s_status.sta_ip, sizeof(s_status.sta_ip),
                     IPSTR, IP2STR(&evt->ip_info.ip));
            xSemaphoreGive(s_mutex);
            APPLOG_I("Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
            softap_set_dns();
            obtain_time();
            s_retry_count = WT_WIFI_STA_RETRY;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Public management API                                                */
/* ------------------------------------------------------------------ */

wt_wifi_status_t wt_wifi_get_status(void)
{
    wt_wifi_status_t snap;
    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snap = s_status;
        /* Refresh RSSI */
        if (s_status.sta_connected)
        {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
                snap.sta_rssi = ap.rssi;
        }
        xSemaphoreGive(s_mutex);
    }
    else
    {
        memset(&snap, 0, sizeof(snap));
    }
    return snap;
}

int wt_wifi_get_profiles(wt_wifi_profile_t *out, int max_count)
{
    if (!out || max_count <= 0 || !s_mutex)
        return 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = (s_profile_cnt < max_count) ? s_profile_cnt : max_count;
    memcpy(out, s_profiles, n * sizeof(wt_wifi_profile_t));
    xSemaphoreGive(s_mutex);
    return n;
}

bool wt_wifi_add_profile(const char *ssid, const char *passwd)
{
    if (!ssid || !s_mutex)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_profile_cnt >= WT_WIFI_MAX_PROFILES)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    strlcpy(s_profiles[s_profile_cnt].ssid, ssid, WT_WIFI_SSID_LEN);
    strlcpy(s_profiles[s_profile_cnt].passwd,
            passwd ? passwd : "", WT_WIFI_PASS_LEN);
    s_profile_cnt++;
    nvs_save_profiles();
    xSemaphoreGive(s_mutex);
    APPLOG_I("Profile added: %s", ssid);
    return true;
}

bool wt_wifi_remove_profile(int index)
{
    if (!s_mutex)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_profile_cnt)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    /* Shift entries down */
    for (int i = index; i < s_profile_cnt - 1; i++)
    {
        s_profiles[i] = s_profiles[i + 1];
    }
    s_profile_cnt--;
    if (s_active_profile >= s_profile_cnt)
        s_active_profile = 0;
    nvs_save_profiles();
    xSemaphoreGive(s_mutex);
    APPLOG_I("Profile %d removed", index);
    return true;
}

bool wt_wifi_connect_profile(int index)
{
    if (!s_mutex)
        return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_profile_cnt)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    apply_sta_profile(index);
    s_retry_count = WT_WIFI_STA_RETRY;
    xSemaphoreGive(s_mutex);
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
    APPLOG_I("Connecting to profile %d: %s", index, s_profiles[index].ssid);
    return true;
}

/* ------------------------------------------------------------------ */
/*  WiFi task                                                            */
/* ------------------------------------------------------------------ */

void wt_task_wifi(void *pvParameters)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* NVS init */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Load STA profiles from NVS */
    nvs_load_profiles();
    APPLOG_I("Loaded %d WiFi profiles", s_profile_cnt);

    /* Register events */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler, NULL, NULL));

    /* WiFi init */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* AP config */
    memset(&s_ap_cfg, 0, sizeof(s_ap_cfg));
    strlcpy((char *)s_ap_cfg.ap.ssid, WT_WIFI_AP_SSID, sizeof(s_ap_cfg.ap.ssid));
    strlcpy((char *)s_ap_cfg.ap.password, WT_WIFI_AP_PASSWD, sizeof(s_ap_cfg.ap.password));
    s_ap_cfg.ap.ssid_len = strlen(WT_WIFI_AP_SSID);
    s_ap_cfg.ap.channel = WT_WIFI_AP_CHANNEL;
    s_ap_cfg.ap.max_connection = WT_WIFI_AP_MAX_CONN;
    s_ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_ap_cfg.ap.pmf_cfg.required = false;

    s_netif_ap = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));

    /* STA config — first profile */
    if (s_profile_cnt > 0)
        apply_sta_profile(0);

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Give the stack a moment before the first connect attempt */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool started = s_status.sta_started;
        bool connected = s_status.sta_connected;
        xSemaphoreGive(s_mutex);

        if (started && !connected)
        {
            if (s_retry_count-- > 0)
            {
                APPLOG_I("WiFi connect attempt (%d/%d) → %s",
                         WT_WIFI_STA_RETRY - s_retry_count,
                         WT_WIFI_STA_RETRY,
                         s_profiles[s_active_profile].ssid);
                esp_wifi_connect();
            }
            else
            {
                /* Rotate to next profile */
                s_retry_count = WT_WIFI_STA_RETRY;
                int next = (s_active_profile + 1) % s_profile_cnt;
                apply_sta_profile(next);
                APPLOG_I("Switching to profile %d: %s", next, s_profiles[next].ssid);
                esp_wifi_connect();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(WT_WIFI_STA_RETRY_WAIT_MS));
    }
}
