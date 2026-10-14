//=============================================================================
/*!
    \file    wt_app_wifi.h
    \brief   WiFi: access point, saved networks, scan, device name.
*/
//=============================================================================

// wt_task_wifi keeps the connection up. The other functions are
// thread-safe and may be called from any task.

#ifndef WT_APP_WIFI_H
#define WT_APP_WIFI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define WT_WIFI_MAX_PROFILES 8  ///< Most saved networks
#define WT_WIFI_SSID_LEN     64 ///< SSID buffer size, including the NUL
#define WT_WIFI_PASS_LEN     64 ///< Password buffer size, including the NUL
#define WT_WIFI_HOSTNAME_LEN 32 ///< Device name buffer size, including the NUL

#define WT_WIFI_DEFAULT_HOSTNAME "watchtower" ///< Default device name ("watchtower.local")

/*!
    \brief  A single WiFi station credential set.
*/
typedef struct
{
    char ssid[WT_WIFI_SSID_LEN];
    char passwd[WT_WIFI_PASS_LEN];
} wt_wifi_profile_t;

/*!
    \brief  Snapshot of current WiFi state (returned by value, thread-safe).
*/
typedef struct
{
    // Station
    bool sta_started;
    bool sta_connected;
    char sta_ip[20];
    char sta_ssid[WT_WIFI_SSID_LEN];
    int8_t sta_rssi;
    uint8_t sta_active_profile; ///< Position of the network in use in the saved list

    // Access point
    bool ap_active;
    char ap_ssid[WT_WIFI_SSID_LEN];
    char ap_ip[20];
    uint8_t ap_clients; ///< Devices connected to the access point

    char hostname[WT_WIFI_HOSTNAME_LEN]; ///< Device name; answers at "<hostname>.local"
} wt_wifi_status_t;

/*!
    \brief  One network found by a scan.
 */
typedef struct
{
    char ssid[33];   ///< Network name, NUL-terminated
    int8_t rssi;     ///< Signal strength in dBm
    uint8_t channel; ///< Primary channel
    bool secure;     ///< false for an open network
} wt_wifi_scan_result_t;

#define WT_WIFI_SCAN_MAX_RESULTS 16 ///< Most networks kept from a scan

void wt_task_wifi(void *pvParameters);

wt_wifi_status_t wt_wifi_get_status(void);

int wt_wifi_get_profiles(wt_wifi_profile_t *out, int max_count);

bool wt_wifi_add_profile(const char *ssid, const char *passwd);

bool wt_wifi_remove_profile(int index);

bool wt_wifi_connect_profile(int index);

uint32_t wt_wifi_get_generation(void);

void wt_wifi_request_ntp_sync(void);

bool wt_wifi_set_hostname(const char *hostname);

bool wt_wifi_set_ap_password(const char *password);

bool wt_wifi_scan_start(void);

int wt_wifi_get_scan_results(wt_wifi_scan_result_t *out, int max_count, bool *scanning);

uint32_t wt_wifi_get_scan_generation(void);

#endif /* WT_APP_WIFI_H */
