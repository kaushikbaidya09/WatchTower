/*!
    \file   wt_app_wifi.c
    \brief  WiFi AP+STA driver with dynamic NVS-backed profile management.

    \details
    Rotates through saved profiles with a bounded retry count per profile,
    reconnecting automatically on disconnect.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
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
#include "wt_app_settings.h"
#include "wt_app_time.h"

#ifndef ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

#define WT_WIFI_AP_SSID "WatchTower"
#define WT_WIFI_AP_PASSWORD "watch@360" ///< WPA2-PSK, must be 8-63 chars
#define WT_WIFI_AP_CHANNEL 6
#define WT_WIFI_AP_MAX_CONN 4
#define WT_WIFI_AP_IP "192.168.4.1"

#define WT_WIFI_STA_RETRY 1
#define WT_WIFI_STA_RETRY_WAIT_MS 10000

#define WT_NVS_NS "wt_wifi"
#define WT_NVS_KEY_CNT "prof_cnt"
#define WT_NVS_KEY_SSID "ssid_%d"
#define WT_NVS_KEY_PASS "pass_%d"

/* DHCP server DNS option */
#define WT_WIFI_DHCPS_OFFER_DNS 0x02

static SemaphoreHandle_t s_mutex = NULL;
static wt_wifi_profile_t s_profiles[WT_WIFI_MAX_PROFILES];
static int s_profile_cnt = 0;
static int s_active_profile = 0;

static wt_wifi_status_t s_status; ///< Protected by s_mutex
static uint32_t s_generation = 0; ///< Bumped on state change; protected by s_mutex

static wifi_config_t s_ap_cfg;
static wifi_config_t s_sta_cfg;
static esp_netif_t *s_netif_ap = NULL;
static esp_netif_t *s_netif_sta = NULL;

static int s_retry_count = WT_WIFI_STA_RETRY;
static TaskHandle_t s_wifi_task = NULL; ///< wt_task_wifi handle, notified to react to events immediately
static TaskHandle_t s_ntp_task = NULL;  ///< Dedicated task for the slow (up to 15 s) NTP sync
static volatile uint8_t s_last_disconnect_reason = 0;

/* NVS helpers operate on a caller-supplied snapshot only no globals, no
 * lock so the (slow) flash write can safely happen outside s_mutex. */

/*!
    \brief  Write the given profile snapshot to NVS (count plus per-index SSID/password).
 */
static void nvs_save_profiles(int cnt, const wt_wifi_profile_t *profiles)
{
    nvs_handle_t h;
    if (nvs_open(WT_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        wt_log_error("nvs_open (RW) failed profiles not persisted");
        return;
    }

    if (nvs_set_i32(h, WT_NVS_KEY_CNT, cnt) != ESP_OK)
    {
        wt_log_error("nvs_set_i32(prof_cnt) failed");
    }

    char key[20];
    for (int i = 0; i < cnt; i++)
    {
        snprintf(key, sizeof(key), WT_NVS_KEY_SSID, i);
        if (nvs_set_str(h, key, profiles[i].ssid) != ESP_OK)
        {
            wt_log_error("nvs_set_str(%s) failed", key);
        }
        snprintf(key, sizeof(key), WT_NVS_KEY_PASS, i);
        if (nvs_set_str(h, key, profiles[i].passwd) != ESP_OK)
        {
            wt_log_error("nvs_set_str(%s) failed", key);
        }
    }
    if (nvs_commit(h) != ESP_OK)
    {
        wt_log_error("nvs_commit(wifi profiles) failed");
    }
    nvs_close(h);
}

/*!
    \brief  Load the profile list from NVS into s_profiles/s_profile_cnt, seeding
            a single default profile if no NVS namespace exists yet.
 */
static void nvs_load_profiles(void)
{
    nvs_handle_t h;
    if (nvs_open(WT_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        /* No stored profiles seed with the one default that has real
           credentials.  Seeding empty slots would make the reconnect
           rotation cycle forever through unusable profiles. */
        s_profile_cnt = 1;
        strlcpy(s_profiles[0].ssid, "Wokwi-GUEST", WT_WIFI_SSID_LEN);
        strlcpy(s_profiles[0].passwd, "", WT_WIFI_PASS_LEN);
        nvs_save_profiles(s_profile_cnt, s_profiles);
        return;
    }

    int32_t cnt = 0;
    if (nvs_get_i32(h, WT_NVS_KEY_CNT, &cnt) != ESP_OK)
    {
        wt_log_error("nvs_get_i32(prof_cnt) failed");
    }
    s_profile_cnt = (cnt > WT_WIFI_MAX_PROFILES) ? WT_WIFI_MAX_PROFILES : (int)cnt;

    char key[20];
    size_t len;
    for (int i = 0; i < s_profile_cnt; i++)
    {
        len = WT_WIFI_SSID_LEN;
        snprintf(key, sizeof(key), WT_NVS_KEY_SSID, i);
        if (nvs_get_str(h, key, s_profiles[i].ssid, &len) != ESP_OK)
        {
            wt_log_error("nvs_get_str(%s) failed", key);
        }

        len = WT_WIFI_PASS_LEN;
        snprintf(key, sizeof(key), WT_NVS_KEY_PASS, i);
        if (nvs_get_str(h, key, s_profiles[i].passwd, &len) != ESP_OK)
        {
            wt_log_error("nvs_get_str(%s) failed", key);
        }
    }
    nvs_close(h);
}

/*!
    \brief  Build the STA wifi_config_t for the profile at index and apply it
            via esp_wifi_set_config(), tracking it as the active profile.
 */
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

/*!
    \brief  Propagate the STA-side DNS server to the SoftAP's DHCP server so
            AP clients can resolve names through the upstream network.
 */
static void softap_set_dns(void)
{
    esp_netif_dns_info_t dns;
    if (esp_netif_get_dns_info(s_netif_sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK)
        return;
    uint8_t opt = WT_WIFI_DHCPS_OFFER_DNS;
    if (esp_netif_dhcps_stop(s_netif_ap) != ESP_OK)
    {
        wt_log_warn("esp_netif_dhcps_stop failed");
    }
    if (esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET,
                               ESP_NETIF_DOMAIN_NAME_SERVER, &opt, sizeof(opt)) != ESP_OK)
    {
        wt_log_warn("esp_netif_dhcps_option failed");
    }
    if (esp_netif_set_dns_info(s_netif_ap, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK)
    {
        wt_log_warn("esp_netif_set_dns_info failed");
    }
    if (esp_netif_dhcps_start(s_netif_ap) != ESP_OK)
    {
        wt_log_warn("esp_netif_dhcps_start failed");
    }
}

/*!
    \brief  Blocking (up to ~15 s) NTP sync runs only on the dedicated
            ntp_sync_task, never inline in the WiFi event-handler callback.
 */
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
        gmtime_r(&now, &ti);
    }

    if (retry < 15)
    {
        wt_log_info("NTP synced: %s", asctime(&ti));

        time(&now);
        wt_log_info("EPOCH TIME %lld", now);

        struct tm utc, local;
        gmtime_r(&now, &utc);

        wt_settings_t cfg = wt_settings_get();
        setenv("TZ", cfg.timezone, 1);
        tzset();
        localtime_r(&now, &local);

        wt_log_info("NTP Time  : %02d/%02d/%04d %02d:%02d:%02d",
                 ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900,
                 ti.tm_hour, ti.tm_min, ti.tm_sec);
        wt_log_info("UTC       : %02d/%02d/%04d %02d:%02d:%02d (UTC)",
                 utc.tm_mday, utc.tm_mon + 1, utc.tm_year + 1900,
                 utc.tm_hour, utc.tm_min, utc.tm_sec);
        wt_log_info("LOCAL     : %02d/%02d/%04d %02d:%02d:%02d (%s)",
                 local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
                 local.tm_hour, local.tm_min, local.tm_sec, tzname[0]);

        wt_time_t rtc_time = {
            .sec = ti.tm_sec,
            .min = ti.tm_min,
            .hour = ti.tm_hour,
            .day = ti.tm_mday,
            .month = ti.tm_mon + 1,
            .year = ti.tm_year + 1900};
        wt_time_set_time(&rtc_time);
    }
    else
    {
        wt_log_warn("NTP sync timed out");
    }
}

/*!
    \brief  Dedicated task for obtain_time() so the (up to ~15 s) NTP poll
            never runs on the WiFi event-handler callback stack.
 */
static void ntp_sync_task(void *pvParameter)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        obtain_time();
    }
}

/*!
    \brief  Classify a WIFI_EVENT_STA_DISCONNECTED reason code.
    \return true if retrying the same profile is pointless (bad credentials/
            AP not found) and the task should rotate to the next profile
            immediately instead of burning the normal retry budget.
 */
static bool wifi_disconnect_reason_is_unrecoverable(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return true;
    default:
        return false;
    }
}

/*!
    \brief  Central WIFI_EVENT/IP_EVENT handler: updates the shared status
            snapshot and generation counter, and wakes the reconnect/NTP tasks.
 */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT)
    {
        switch (id)
        {
        case WIFI_EVENT_STA_START:
            wt_log_info("STA started");
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.sta_started = true;
            s_generation++;
            xSemaphoreGive(s_mutex);
            break;

        case WIFI_EVENT_STA_STOP:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.sta_started = false;
            s_status.sta_connected = false;
            s_generation++;
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
            s_generation++;
            xSemaphoreGive(s_mutex);
            wt_log_info("STA connected: %.*s", evt->ssid_len, evt->ssid);
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED:
        {
            wifi_event_sta_disconnected_t *evt = (wifi_event_sta_disconnected_t *)data;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.sta_connected = false;
            memset(s_status.sta_ip, 0, sizeof(s_status.sta_ip));
            s_generation++;
            xSemaphoreGive(s_mutex);
            s_last_disconnect_reason = evt->reason;
            if (s_wifi_task)
            {
                /* Wake the reconnect task immediately instead of waiting
                   for its fixed poll interval. */
                xTaskNotifyGive(s_wifi_task);
            }
            break;
        }

        case WIFI_EVENT_AP_START:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.ap_active = true;
            strlcpy(s_status.ap_ssid, WT_WIFI_AP_SSID, WT_WIFI_SSID_LEN);
            strlcpy(s_status.ap_ip, WT_WIFI_AP_IP, sizeof(s_status.ap_ip));
            s_generation++;
            xSemaphoreGive(s_mutex);
            wt_log_info("AP started: %s", WT_WIFI_AP_SSID);
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.ap_clients++;
            s_generation++;
            xSemaphoreGive(s_mutex);
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            if (s_status.ap_clients > 0)
                s_status.ap_clients--;
            s_generation++;
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
            s_generation++;
            xSemaphoreGive(s_mutex);
            wt_log_info("Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
            softap_set_dns();
            if (s_ntp_task)
            {
                xTaskNotifyGive(s_ntp_task);
            }
            s_retry_count = WT_WIFI_STA_RETRY;
        }
    }
}

/*!
    \brief  Get a snapshot of current WiFi state.
 */
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

/*!
    \brief  Copy the stored profile list into caller-supplied array.
    \param[out] out        Destination array of at least max_count entries.
    \param[in]  max_count  Maximum profiles to copy.
    \return Number of profiles copied.
 */
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

/*!
    \brief  Add a new STA profile (persisted to NVS).
    \param[in]  ssid    SSID of the new profile.
    \param[in]  passwd  Passphrase for the new profile (may be empty/NULL for open networks).
    \return true on success, false if the list is full or args are invalid.
 */
bool wt_wifi_add_profile(const char *ssid, const char *passwd)
{
    if (!ssid || !s_mutex)
        return false;

    wt_wifi_profile_t snapshot[WT_WIFI_MAX_PROFILES];
    int cnt;

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
    cnt = s_profile_cnt;
    memcpy(snapshot, s_profiles, (size_t)cnt * sizeof(wt_wifi_profile_t));
    s_generation++;
    xSemaphoreGive(s_mutex);

    /* Flash write happens outside the lock so it never blocks
       wifi_event_handler from processing connect/disconnect events. */
    nvs_save_profiles(cnt, snapshot);
    wt_log_info("Profile added: %s", ssid);
    return true;
}

/*!
    \brief  Remove profile at index (persisted to NVS).
    \param[in]  index  Index of the profile to remove.
    \return true on success.
 */
bool wt_wifi_remove_profile(int index)
{
    if (!s_mutex)
        return false;

    wt_wifi_profile_t snapshot[WT_WIFI_MAX_PROFILES];
    int cnt;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_profile_cnt)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    if (s_profile_cnt <= 1)
    {
        /* Refuse to remove the last profile the reconnect task divides
           by s_profile_cnt when rotating, so it must never reach zero. */
        xSemaphoreGive(s_mutex);
        wt_log_warn("Refusing to remove the last WiFi profile");
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
    cnt = s_profile_cnt;
    memcpy(snapshot, s_profiles, (size_t)cnt * sizeof(wt_wifi_profile_t));
    s_generation++;
    xSemaphoreGive(s_mutex);

    nvs_save_profiles(cnt, snapshot);
    wt_log_info("Profile %d removed", index);
    return true;
}

/*!
    \brief  Monotonic counter incremented whenever WiFi state that matters to
            UI clients changes (profile list edits, STA connect/disconnect,
            AP client count, IP address). Does NOT change on RSSI drift alone,
            so a caller can use it to decide "has anything worth re-pushing
            changed" instead of resending the whole WiFi object on a timer.
 */
uint32_t wt_wifi_get_generation(void)
{
    uint32_t gen = 0;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        gen = s_generation;
        xSemaphoreGive(s_mutex);
    }
    return gen;
}

/*!
    \brief  Immediately connect to the profile at index.
            Disconnects any current session first.
    \param[in]  index  Index of the profile to connect to.
    \return true if the request was accepted (async connection may fail).
 */
bool wt_wifi_connect_profile(int index)
{
    if (!s_mutex)
        return false;

    char ssid[WT_WIFI_SSID_LEN];

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (index < 0 || index >= s_profile_cnt)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    apply_sta_profile(index);
    s_retry_count = WT_WIFI_STA_RETRY;
    strlcpy(ssid, s_profiles[index].ssid, sizeof(ssid));
    s_generation++;
    xSemaphoreGive(s_mutex);

    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_wifi_connect();
    wt_log_info("Connecting to profile %d: %s", index, ssid);
    return true;
}

/*!
    \brief  FreeRTOS task pin to Core 0.
    \param[in]  pvParameters  Unused (required by the FreeRTOS task function signature).
 */
void wt_task_wifi(void *pvParameters)
{
    // wt_log_info("---------- WIFI TASK STARTED ----------");

    s_wifi_task = xTaskGetCurrentTaskHandle();
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Load STA profiles from NVS */
    nvs_load_profiles();
    wt_log_info("Loaded %d WiFi profiles", s_profile_cnt);

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
    strlcpy((char *)s_ap_cfg.ap.password, WT_WIFI_AP_PASSWORD, sizeof(s_ap_cfg.ap.password));
    s_ap_cfg.ap.ssid_len = strlen(WT_WIFI_AP_SSID);
    s_ap_cfg.ap.channel = WT_WIFI_AP_CHANNEL;
    s_ap_cfg.ap.max_connection = WT_WIFI_AP_MAX_CONN;
    s_ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_ap_cfg.ap.pmf_cfg.required = false;

    s_netif_ap = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
    wt_log_info("AP started: %s / %s", WT_WIFI_AP_SSID, WT_WIFI_AP_PASSWORD);

    /* STA config first profile */
    if (s_profile_cnt > 0)
        apply_sta_profile(0);

    ESP_ERROR_CHECK(esp_wifi_start());

    xTaskCreate(ntp_sync_task, "WT_NTP", 4096, NULL, 3, &s_ntp_task);

    /* Give the stack a moment before the first connect attempt */
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1)
    {
        char active_ssid[WT_WIFI_SSID_LEN];
        bool started;
        bool connected;
        int cnt;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        started = s_status.sta_started;
        connected = s_status.sta_connected;
        cnt = s_profile_cnt;
        if (cnt > 0)
        {
            strlcpy(active_ssid, s_profiles[s_active_profile].ssid, sizeof(active_ssid));
        }
        xSemaphoreGive(s_mutex);

        /* cnt > 0 guard is defense-in-depth: wt_wifi_remove_profile() already
           refuses to remove the last profile, so this should never be 0. */
        if (started && !connected && cnt > 0)
        {
            uint8_t reason = s_last_disconnect_reason;
            bool unrecoverable = wifi_disconnect_reason_is_unrecoverable(reason);
            int retry_before = s_retry_count--;
            bool rotate_now = unrecoverable || (retry_before <= 0);

            if (!rotate_now)
            {
                wt_log_info("WiFi connect attempt (%d/%d) → %s",
                         WT_WIFI_STA_RETRY - s_retry_count,
                         WT_WIFI_STA_RETRY, active_ssid);
                esp_wifi_connect();
            }
            else
            {
                /* Rotate to next profile */
                s_retry_count = WT_WIFI_STA_RETRY;

                xSemaphoreTake(s_mutex, portMAX_DELAY);
                int next = (s_active_profile + 1) % s_profile_cnt;
                apply_sta_profile(next);
                strlcpy(active_ssid, s_profiles[next].ssid, sizeof(active_ssid));
                xSemaphoreGive(s_mutex);

                wt_log_info("Switching to profile %d: %s (reason=%u)", next, active_ssid, reason);
                esp_wifi_connect();
            }
        }

        /* Blocks up to WT_WIFI_STA_RETRY_WAIT_MS, but wifi_event_handler
           wakes this task immediately on WIFI_EVENT_STA_DISCONNECTED. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WT_WIFI_STA_RETRY_WAIT_MS));
    }
}
