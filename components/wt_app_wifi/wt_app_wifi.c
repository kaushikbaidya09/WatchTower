//=============================================================================
/*!
    \file    wt_app_wifi.c
    \brief   WiFi: access point, saved networks, scan, device name and NTP.
*/
//=============================================================================

// Access point and station run together. The station tries each saved
// network in turn and reconnects when the link drops.

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
#include "mdns.h"
#include "wt_app_wifi.h"
#include "wt_app_log.h"
#include "wt_app_settings.h"
#include "wt_app_time.h"

#ifndef ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

#define WT_WIFI_AP_SSID     "WatchTower"
#define WT_WIFI_AP_PASSWORD "watch@360" ///< Default access point password
#define WT_WIFI_AP_PASS_MIN 8           ///< Shortest WPA2 passphrase
#define WT_WIFI_AP_PASS_MAX 63          ///< Longest WPA2 passphrase
#define WT_WIFI_SSID_MAX    32          ///< Longest SSID the radio accepts
#define WT_WIFI_AP_CHANNEL  6
#define WT_WIFI_AP_MAX_CONN 4
#define WT_WIFI_AP_IP       "192.168.4.1"

#define WT_WIFI_STA_RETRY         1
#define WT_WIFI_STA_RETRY_WAIT_MS 10000
#define WT_WIFI_BACKOFF_MIN_MS    2000 ///< Pause before the second consecutive failed connect attempt
#define WT_WIFI_BACKOFF_MAX_MS    30000 ///< Ceiling the pause doubles up to

#define WT_NVS_NS           "wt_wifi"
#define WT_NVS_KEY_CNT      "prof_cnt"
#define WT_NVS_KEY_SSID     "ssid_%d"
#define WT_NVS_KEY_PASS     "pass_%d"
#define WT_NVS_KEY_HOSTNAME "hostname"
#define WT_NVS_KEY_AP_PASS  "ap_pass"

// DHCP server option: offer a DNS server to access point clients.
#define WT_WIFI_DHCPS_OFFER_DNS 0x02

static SemaphoreHandle_t s_mutex = NULL;
static wt_wifi_profile_t s_profiles[WT_WIFI_MAX_PROFILES];
static int s_profile_cnt    = 0;
static int s_active_profile = 0;

static wt_wifi_status_t s_status; ///< Protected by s_mutex
static uint32_t s_generation = 0; ///< Increases on every change; protected by s_mutex

static wifi_config_t s_ap_cfg;
static wifi_config_t s_sta_cfg;
static esp_netif_t *s_netif_ap  = NULL;
static esp_netif_t *s_netif_sta = NULL;

static int s_retry_count                         = WT_WIFI_STA_RETRY;
static TaskHandle_t s_wifi_task                  = NULL; ///< WiFi task, woken on events
static TaskHandle_t s_ntp_task                   = NULL; ///< NTP task
static volatile uint8_t s_last_disconnect_reason = 0;
static volatile uint32_t s_fail_streak = 0; ///< Consecutive connect attempts without getting an IP

#define WT_WIFI_SCAN_RAW_MAX    24    ///< Raw scan records read from the driver
#define WT_WIFI_SCAN_TIMEOUT_MS 12000 ///< A scan is abandoned after this long

static wt_wifi_scan_result_t s_scan_results[WT_WIFI_SCAN_MAX_RESULTS]; ///< Protected by s_mutex
static int s_scan_count = 0; ///< Entries in s_scan_results; protected by s_mutex
static uint32_t s_scan_generation =
    0; ///< Increases when a scan starts or ends; protected by s_mutex
static volatile bool s_scanning                = false;       ///< A scan is running
static volatile TickType_t s_scan_started_tick = 0;           ///< Tick count when it started
static wifi_ap_record_t s_scan_records[WT_WIFI_SCAN_RAW_MAX]; ///< Used by the event task only

static char s_hostname[WT_WIFI_HOSTNAME_LEN] = WT_WIFI_DEFAULT_HOSTNAME; ///< Protected by s_mutex
static char s_ap_password[WT_WIFI_PASS_LEN]  = WT_WIFI_AP_PASSWORD;      ///< Protected by s_mutex

// The NVS helpers work on a copy passed by the caller, so the slow flash
// write can run without holding s_mutex.

/*!
    \brief  Save the network list to NVS.
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
    // Erase unused slots so removed credentials do not remain in flash.
    for (int i = cnt; i < WT_WIFI_MAX_PROFILES; i++)
    {
        snprintf(key, sizeof(key), WT_NVS_KEY_SSID, i);
        (void)nvs_erase_key(h, key);
        snprintf(key, sizeof(key), WT_NVS_KEY_PASS, i);
        (void)nvs_erase_key(h, key);
    }

    if (nvs_commit(h) != ESP_OK)
    {
        wt_log_error("nvs_commit(wifi profiles) failed");
    }
    nvs_close(h);
}

/*!
    \brief  Save one string (device name, access point password) to NVS.
*/
static bool nvs_save_string(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(WT_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        wt_log_error("nvs_open (RW) failed %s not persisted", key);
        return false;
    }

    bool ok = (nvs_set_str(h, key, value) == ESP_OK) && (nvs_commit(h) == ESP_OK);
    if (!ok)
    {
        wt_log_error("Saving %s to NVS failed", key);
    }
    nvs_close(h);
    return ok;
}

/*!
    \brief  true if name can be used as a device name: 1-31 characters from
            a-z, 0-9 and '-', not starting or ending with '-'.
*/
static bool hostname_is_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len == 0 || len >= WT_WIFI_HOSTNAME_LEN || name[0] == '-' || name[len - 1] == '-')
    {
        return false;
    }
    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
        {
            return false;
        }
    }
    return true;
}

/*!
    \brief  true if password has a valid WPA2 length (8-63 characters).
*/
static bool ap_password_is_valid(const char *password)
{
    size_t len = password ? strlen(password) : 0;
    return (len >= WT_WIFI_AP_PASS_MIN) && (len <= WT_WIFI_AP_PASS_MAX);
}

/*!
    \brief  Load the device name and access point password; anything missing
            or invalid keeps its default.
*/
static void nvs_load_device_config(void)
{
    nvs_handle_t h;
    if (nvs_open(WT_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return;
    }

    char value[WT_WIFI_PASS_LEN];
    size_t len = sizeof(value);
    if (nvs_get_str(h, WT_NVS_KEY_HOSTNAME, value, &len) == ESP_OK && hostname_is_valid(value))
    {
        strlcpy(s_hostname, value, sizeof(s_hostname));
    }

    len = sizeof(value);
    if (nvs_get_str(h, WT_NVS_KEY_AP_PASS, value, &len) == ESP_OK && ap_password_is_valid(value))
    {
        strlcpy(s_ap_password, value, sizeof(s_ap_password));
    }
    memset(value, 0, sizeof(value)); // clear the password copy
    nvs_close(h);
}

/*!
    \brief  Load the saved networks. On first boot a single default network
            is stored.
*/
static void nvs_load_profiles(void)
{
    nvs_handle_t h;
    if (nvs_open(WT_NVS_NS, NVS_READONLY, &h) != ESP_OK)
    {
        // Nothing stored yet: start with the one default network.
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
    if (cnt < 0)
    {
        cnt = 0;
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
    \brief  Configure the station for the saved network at index.
*/
static void apply_sta_profile(int index)
{
    memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
    strlcpy((char *)s_sta_cfg.sta.ssid, s_profiles[index].ssid, sizeof(s_sta_cfg.sta.ssid));
    strlcpy((char *)s_sta_cfg.sta.password, s_profiles[index].passwd,
            sizeof(s_sta_cfg.sta.password));
    s_sta_cfg.sta.scan_method        = WIFI_ALL_CHANNEL_SCAN;
    s_sta_cfg.sta.failure_retry_cnt  = WT_WIFI_STA_RETRY;
    s_sta_cfg.sta.threshold.authmode = (strlen(s_profiles[index].passwd) == 0)
                                           ? WIFI_AUTH_OPEN
                                           : ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD;
    s_sta_cfg.sta.sae_pwe_h2e        = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
    s_active_profile = index;
}

/*!
    \brief  Give access point clients the DNS server of the joined network.
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
    if (esp_netif_dhcps_option(s_netif_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &opt,
                               sizeof(opt)) != ESP_OK)
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
    \brief  Store the results of a finished scan: hidden networks left out, each
            network once (strongest signal), strongest first.
 */
static void scan_collect_results(void)
{
    uint16_t found = WT_WIFI_SCAN_RAW_MAX;
    if (esp_wifi_scan_get_ap_records(&found, s_scan_records) != ESP_OK)
    {
        found = 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scan_count = 0;
    // Records arrive strongest first, so the first one seen for an SSID is
    // the one to keep.
    for (uint16_t i = 0; i < found && s_scan_count < WT_WIFI_SCAN_MAX_RESULTS; i++)
    {
        const char *ssid = (const char *)s_scan_records[i].ssid;
        if (ssid[0] == '\0')
        {
            continue;
        }

        bool duplicate = false;
        for (int k = 0; k < s_scan_count; k++)
        {
            if (strncmp(s_scan_results[k].ssid, ssid, sizeof(s_scan_results[k].ssid)) == 0)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }

        wt_wifi_scan_result_t *r = &s_scan_results[s_scan_count++];
        strlcpy(r->ssid, ssid, sizeof(r->ssid));
        r->rssi    = s_scan_records[i].rssi;
        r->channel = s_scan_records[i].primary;
        r->secure  = (s_scan_records[i].authmode != WIFI_AUTH_OPEN);
    }
    s_scanning = false;
    s_scan_generation++;
    int kept = s_scan_count;
    xSemaphoreGive(s_mutex);

    wt_log_info("WiFi scan done: %d network(s)", kept);
}

/*!
    \brief  Handles WiFi and IP events: updates the status and wakes the
            reconnect and NTP tasks.
*/
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
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
                s_status.sta_started   = false;
                s_status.sta_connected = false;
                s_generation++;
                xSemaphoreGive(s_mutex);
                break;

            case WIFI_EVENT_STA_CONNECTED:
            {
                const wifi_event_sta_connected_t *evt = (const wifi_event_sta_connected_t *)data;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_status.sta_connected      = true;
                s_status.sta_active_profile = (uint8_t)s_active_profile;
                snprintf(s_status.sta_ssid, WT_WIFI_SSID_LEN, "%.*s", evt->ssid_len, evt->ssid);
                s_generation++;
                xSemaphoreGive(s_mutex);
                wt_log_info("STA connected: %.*s", evt->ssid_len, evt->ssid);
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED:
            {
                const wifi_event_sta_disconnected_t *evt =
                    (const wifi_event_sta_disconnected_t *)data;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                s_status.sta_connected = false;
                memset(s_status.sta_ip, 0, sizeof(s_status.sta_ip));
                s_generation++;
                xSemaphoreGive(s_mutex);
                s_last_disconnect_reason = evt->reason;
                if (s_wifi_task)
                {
                    // Wake the reconnect task now.
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

            case WIFI_EVENT_SCAN_DONE:
                // Ignore scans that the console did not ask for.
                if (s_scanning)
                {
                    scan_collect_results();
                    if (s_wifi_task)
                    {
                        // Let the reconnect task resume now.
                        xTaskNotifyGive(s_wifi_task);
                    }
                }
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
            snprintf(s_status.sta_ip, sizeof(s_status.sta_ip), IPSTR, IP2STR(&evt->ip_info.ip));
            s_generation++;
            xSemaphoreGive(s_mutex);
            wt_log_info("Got IP: " IPSTR, IP2STR(&evt->ip_info.ip));
            softap_set_dns();
            if (s_ntp_task)
            {
                xTaskNotifyGive(s_ntp_task);
            }
            s_retry_count = WT_WIFI_STA_RETRY;
            s_fail_streak = 0;
        }
    }
}

/*!
    \brief  Get a copy of the current WiFi status.
*/
wt_wifi_status_t wt_wifi_get_status(void)
{
    wt_wifi_status_t snap;
    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        snap = s_status;
        strlcpy(snap.hostname, s_hostname, sizeof(snap.hostname));
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
    \brief  Copy out the saved networks.
    \param[out] out        Array with room for max_count entries.
    \param[in]  max_count  Most entries to copy.
    \return Number of networks copied.
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
    \brief  Save a new network.
    \param[in]  ssid    Network name.
    \param[in]  passwd  Password; empty or NULL for an open network.
    \return true if saved; false if the list is full or a value is invalid.
*/
bool wt_wifi_add_profile(const char *ssid, const char *passwd)
{
    if (!ssid || !s_mutex)
        return false;

    // Reject lengths the radio cannot use: SSID 1-32 bytes, password 8-63
    // characters or empty for an open network.
    size_t ssid_len = strlen(ssid);
    size_t pass_len = passwd ? strlen(passwd) : 0;
    if (ssid_len == 0 || ssid_len > WT_WIFI_SSID_MAX ||
        (pass_len != 0 && (pass_len < WT_WIFI_AP_PASS_MIN || pass_len > WT_WIFI_AP_PASS_MAX)))
    {
        wt_log_warn("Profile rejected: SSID or password length not valid");
        return false;
    }

    wt_wifi_profile_t snapshot[WT_WIFI_MAX_PROFILES];
    int cnt;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_profile_cnt >= WT_WIFI_MAX_PROFILES)
    {
        xSemaphoreGive(s_mutex);
        return false;
    }
    strlcpy(s_profiles[s_profile_cnt].ssid, ssid, WT_WIFI_SSID_LEN);
    strlcpy(s_profiles[s_profile_cnt].passwd, passwd ? passwd : "", WT_WIFI_PASS_LEN);
    s_profile_cnt++;
    cnt = s_profile_cnt;
    memcpy(snapshot, s_profiles, (size_t)cnt * sizeof(wt_wifi_profile_t));
    s_generation++;
    xSemaphoreGive(s_mutex);

    // Saved outside the lock so WiFi events are not held up.
    nvs_save_profiles(cnt, snapshot);
    wt_log_info("Profile added: %s", ssid);
    return true;
}

/*!
    \brief  Remove (forget) a saved network. If it is the one in use, the device
            disconnects from it.
    \param[in]  index  Position of the network in the list.
    \return true if removed.
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
    // The last network may be removed too; the access point keeps the
    // device reachable.
    bool removed_active = (index == s_active_profile);

    for (int i = index; i < s_profile_cnt - 1; i++)
    {
        s_profiles[i] = s_profiles[i + 1];
    }
    s_profile_cnt--;
    memset(&s_profiles[s_profile_cnt], 0, sizeof(s_profiles[s_profile_cnt])); // clear the password

    // Keep s_active_profile on the same network as before.
    if (index < s_active_profile)
    {
        s_active_profile--;
    }
    if (s_active_profile >= s_profile_cnt)
    {
        s_active_profile = 0;
    }

    if (removed_active)
    {
        if (s_profile_cnt > 0)
        {
            apply_sta_profile(s_active_profile);
        }
        else
        {
            // No networks left: clear the station config so the driver
            // stops reconnecting.
            memset(&s_sta_cfg, 0, sizeof(s_sta_cfg));
            esp_wifi_set_config(WIFI_IF_STA, &s_sta_cfg);
        }
        s_retry_count = WT_WIFI_STA_RETRY;
        s_fail_streak = 0;
    }

    cnt = s_profile_cnt;
    memcpy(snapshot, s_profiles, (size_t)cnt * sizeof(wt_wifi_profile_t));
    s_generation++;
    xSemaphoreGive(s_mutex);

    nvs_save_profiles(cnt, snapshot);
    memset(snapshot, 0, sizeof(snapshot));
    wt_log_info("Profile %d removed", index);

    if (removed_active)
    {
        // Disconnect from the removed network; the reconnect task moves on
        // to the next one, if any.
        esp_wifi_disconnect();
    }
    return true;
}

/*!
    \brief  Counter that increases whenever WiFi state shown in the console
            changes (saved networks, connection, access point clients, IP
            address). Signal strength alone does not change it.
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
    \brief  Connect to a saved network now, leaving the current one.
    \param[in]  index  Position of the network in the list.
    \return true if the connection attempt was started.
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
    s_fail_streak = 0;
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
    \brief  Start scanning for nearby networks. Returns at once; the results
            follow a few seconds later.
    \return true if a scan is running; false if it could not start.
 */
bool wt_wifi_scan_start(void)
{
    if (!s_mutex)
    {
        return false;
    }
    if (s_scanning)
    {
        return true;
    }

    // Set first so the reconnect task holds off: the radio cannot scan
    // while connecting.
    s_scanning          = true;
    s_scan_started_tick = xTaskGetTickCount();

    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err == ESP_ERR_WIFI_STATE)
    {
        // A connection attempt is in progress. The scan takes priority:
        // stop the attempt and try once more.
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
        err = esp_wifi_scan_start(NULL, false);
    }
    if (err != ESP_OK)
    {
        s_scanning = false;
        wt_log_warn("WiFi scan could not start: %s", esp_err_to_name(err));
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_scan_generation++; // tells clients a scan is running
    xSemaphoreGive(s_mutex);
    wt_log_info("WiFi scan started");
    return true;
}

/*!
    \brief  Copy out the networks found by the last scan, strongest first.
    \param[out] out        Array with room for max_count entries.
    \param[in]  max_count  Most entries to copy.
    \param[out] scanning   If not NULL, receives true while a scan is running.
    \return Number of networks copied.
 */
int wt_wifi_get_scan_results(wt_wifi_scan_result_t *out, int max_count, bool *scanning)
{
    if (scanning)
    {
        *scanning = s_scanning;
    }
    if (!out || max_count <= 0 || !s_mutex)
    {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = (s_scan_count < max_count) ? s_scan_count : max_count;
    memcpy(out, s_scan_results, (size_t)n * sizeof(wt_wifi_scan_result_t));
    xSemaphoreGive(s_mutex);
    return n;
}

/*!
    \brief  Counter that increases when a scan starts and when it finishes.
 */
uint32_t wt_wifi_get_scan_generation(void)
{
    uint32_t gen = 0;

    if (s_mutex)
    {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        gen = s_scan_generation;
        xSemaphoreGive(s_mutex);
    }
    return gen;
}

/*!
    \brief  Start an NTP sync now. Returns at once.
*/
void wt_wifi_request_ntp_sync(void)
{
    if (s_ntp_task)
    {
        xTaskNotifyGive(s_ntp_task);
    }
}

/*!
    \brief  Change the device name: the device then answers at
            "<name>.local". Applied at once and saved.
    \param[in]  hostname  1-31 characters from a-z, 0-9 and '-', not starting or ending with '-'.
    \return true if the name was valid and applied.
*/
bool wt_wifi_set_hostname(const char *hostname)
{
    if (!s_mutex || !hostname_is_valid(hostname))
    {
        return false;
    }
    if (mdns_hostname_set(hostname) != ESP_OK)
    {
        wt_log_error("mDNS rejected the host name");
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_hostname, hostname, sizeof(s_hostname));
    s_generation++;
    xSemaphoreGive(s_mutex);

    nvs_save_string(WT_NVS_KEY_HOSTNAME, hostname);
    wt_log_info("Device name: %s.local", hostname);
    return true;
}

/*!
    \brief  Change the access point password. Applied at once (connected
            clients have to rejoin) and saved.
    \param[in]  password  8 to 63 characters.
    \return true if the password was valid and applied.
*/
bool wt_wifi_set_ap_password(const char *password)
{
    if (!s_mutex || !ap_password_is_valid(password))
    {
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_ap_password, password, sizeof(s_ap_password));
    memset(s_ap_cfg.ap.password, 0, sizeof(s_ap_cfg.ap.password));
    strlcpy((char *)s_ap_cfg.ap.password, password, sizeof(s_ap_cfg.ap.password));
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg);
    s_generation++;
    xSemaphoreGive(s_mutex);

    if (err != ESP_OK)
    {
        wt_log_error("Applying the AP password failed: %s", esp_err_to_name(err));
        return false;
    }

    nvs_save_string(WT_NVS_KEY_AP_PASS, password);
    wt_log_info("Access point password changed");
    return true;
}

/*!
    \brief  NTP task: each time it is woken, synchronizes the clock over NTP
            and stores the time in the RTC. A sync takes up to 15 s, so it
            does not run on the WiFi task.
*/
static void ntp_sync_task(void *pvParameter)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // The configured server is tried first, then the fallbacks. SNTP keeps
        // the pointer, so the name is stored in a static buffer.
        static char ntp_server[sizeof(((wt_settings_t *)0)->ntp_server)];
        wt_settings_t ntp_cfg = wt_settings_get();

        esp_sntp_stop();
        strlcpy(ntp_server, ntp_cfg.ntp_server, sizeof(ntp_server));
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, ntp_server);
        esp_sntp_setservername(1, "pool.ntp.org");
        esp_sntp_setservername(2, "time.google.com");
        esp_sntp_init();
        wt_log_info("NTP sync started (server: %s)", ntp_server);

        // Reset the status so an earlier sync is not mistaken for this one.
        sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);

        int retry = 0;
        while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && ++retry < 15)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (retry >= 15)
        {
            wt_log_warn("NTP sync timed out");
            continue;
        }

        time_t now = time(NULL);
        struct tm utc, local;
        gmtime_r(&now, &utc);
        wt_settings_t cfg = wt_settings_get();
        setenv("TZ", cfg.timezone, 1);
        tzset();
        localtime_r(&now, &local);
        wt_log_info("NTP synced: %02d/%02d/%04d %02d:%02d:%02d UTC, %02d:%02d:%02d %s", utc.tm_mday,
                    utc.tm_mon + 1, utc.tm_year + 1900, utc.tm_hour, utc.tm_min, utc.tm_sec,
                    local.tm_hour, local.tm_min, local.tm_sec, tzname[0]);

        // The RTC keeps UTC.
        wt_time_t rtc_time = {.sec   = utc.tm_sec,
                              .min   = utc.tm_min,
                              .hour  = utc.tm_hour,
                              .day   = utc.tm_mday,
                              .month = utc.tm_mon + 1,
                              .year  = utc.tm_year + 1900};
        wt_time_set_time(&rtc_time);
    }
}

/*!
    \brief  WiFi task: starts the access point and station, then keeps the
            station connected to a saved network.
    \param[in]  pvParameters  Unused.
*/
void wt_task_wifi(void *pvParameters)
{

    s_wifi_task = xTaskGetCurrentTaskHandle();
    s_mutex     = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    nvs_load_profiles();
    nvs_load_device_config();
    wt_log_info("Loaded %d WiFi profiles", s_profile_cnt);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    memset(&s_ap_cfg, 0, sizeof(s_ap_cfg));
    strlcpy((char *)s_ap_cfg.ap.ssid, WT_WIFI_AP_SSID, sizeof(s_ap_cfg.ap.ssid));
    strlcpy((char *)s_ap_cfg.ap.password, s_ap_password, sizeof(s_ap_cfg.ap.password));
    s_ap_cfg.ap.ssid_len         = strlen(WT_WIFI_AP_SSID);
    s_ap_cfg.ap.channel          = WT_WIFI_AP_CHANNEL;
    s_ap_cfg.ap.max_connection   = WT_WIFI_AP_MAX_CONN;
    s_ap_cfg.ap.authmode         = WIFI_AUTH_WPA2_PSK;
    s_ap_cfg.ap.pmf_cfg.required = false;

    s_netif_ap  = esp_netif_create_default_wifi_ap();
    s_netif_sta = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_cfg));
    // The password is not logged: the log is visible in the web console.

    if (s_profile_cnt > 0)
        apply_sta_profile(0);

    ESP_ERROR_CHECK(esp_wifi_start());

    // mDNS lets the device be found as "<name>.local"; without it the device
    // is still reachable by IP address.
    if (mdns_init() == ESP_OK && mdns_hostname_set(s_hostname) == ESP_OK)
    {
        mdns_instance_name_set("WatchTower");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        wt_log_info("Device name: %s.local", s_hostname);
    }
    else
    {
        wt_log_warn("mDNS unavailable; reach the device by IP address");
    }

    xTaskCreate(ntp_sync_task, "WT_NTP", 4096, NULL, 3, &s_ntp_task);

    // Let the WiFi stack settle before the first connect attempt.
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1)
    {
        char active_ssid[WT_WIFI_SSID_LEN];
        bool started;
        bool connected;
        int cnt;

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        started   = s_status.sta_started;
        connected = s_status.sta_connected;
        cnt       = s_profile_cnt;
        if (cnt > 0)
        {
            strlcpy(active_ssid, s_profiles[s_active_profile].ssid, sizeof(active_ssid));
        }
        xSemaphoreGive(s_mutex);

        // With no saved networks there is nothing to join.
        // A scan requested from the console has the radio until it finishes
        // or times out.
        if (s_scanning)
        {
            if ((xTaskGetTickCount() - s_scan_started_tick) <
                pdMS_TO_TICKS(WT_WIFI_SCAN_TIMEOUT_MS))
            {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
                continue;
            }
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_scanning = false;
            s_scan_generation++;
            xSemaphoreGive(s_mutex);
            wt_log_warn("WiFi scan timed out");
        }

        if (started && !connected && cnt > 0)
        {
            // Wait longer after each failure. Every attempt scans all
            // channels, which interrupts the access point for its clients.
            uint32_t streak = s_fail_streak;
            if (streak > 0)
            {
                uint32_t shift   = (streak > 5) ? 4 : (streak - 1);
                uint32_t wait_ms = WT_WIFI_BACKOFF_MIN_MS << shift;
                if (wait_ms > WT_WIFI_BACKOFF_MAX_MS)
                {
                    wait_ms = WT_WIFI_BACKOFF_MAX_MS;
                }
                vTaskDelay(pdMS_TO_TICKS(wait_ms));

                // Something changed while waiting (connect request, profile
                // edit): start again.
                if (s_fail_streak == 0)
                {
                    continue;
                }
            }
            s_fail_streak = streak + 1;

            // Retrying is pointless for a wrong password or a network that
            // is not there: move on to the next saved network at once.
            uint8_t reason = s_last_disconnect_reason;
            bool unrecoverable =
                (reason == WIFI_REASON_NO_AP_FOUND) || (reason == WIFI_REASON_AUTH_FAIL) ||
                (reason == WIFI_REASON_AUTH_EXPIRE) || (reason == WIFI_REASON_ASSOC_FAIL) ||
                (reason == WIFI_REASON_HANDSHAKE_TIMEOUT);
            int retry_before = s_retry_count--;
            bool rotate_now  = unrecoverable || (retry_before <= 0);

            if (!rotate_now)
            {
                wt_log_info("WiFi connect attempt (%d/%d) → %s", WT_WIFI_STA_RETRY - s_retry_count,
                            WT_WIFI_STA_RETRY, active_ssid);
                esp_wifi_connect();
            }
            else
            {
                // Move on to the next saved network.
                s_retry_count = WT_WIFI_STA_RETRY;

                int next = -1;
                xSemaphoreTake(s_mutex, portMAX_DELAY);
                // The list may have been emptied in the meantime.
                if (s_profile_cnt > 0)
                {
                    next = (s_active_profile + 1) % s_profile_cnt;
                    apply_sta_profile(next);
                    strlcpy(active_ssid, s_profiles[next].ssid, sizeof(active_ssid));
                }
                xSemaphoreGive(s_mutex);

                if (next >= 0)
                {
                    wt_log_info("Switching to profile %d: %s (reason=%u)", next, active_ssid,
                                reason);
                    esp_wifi_connect();
                }
            }
        }

        // Sleeps until the retry interval passes or a disconnect wakes it.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WT_WIFI_STA_RETRY_WAIT_MS));
    }
}
