/*!
    \file   wt_app_wifi.h
    \brief  WiFi driver — AP+STA combined mode with NVS profile management.

    The WiFi task (wt_task_wifi) handles connection logic.
    The management API below is thread-safe and may be called from any task,
    including the HTTP server task.
 */
#ifndef WT_APP_WIFI_H
#define WT_APP_WIFI_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                            */
/* ------------------------------------------------------------------ */
#define WT_WIFI_MAX_PROFILES 8 /*!< Maximum stored STA profiles     */
#define WT_WIFI_SSID_LEN 64    /*!< Max SSID length (incl. '\0')    */
#define WT_WIFI_PASS_LEN 64    /*!< Max password length (incl. '\0')*/

/* ------------------------------------------------------------------ */
/*  Data types                                                           */
/* ------------------------------------------------------------------ */

/** A single WiFi station credential set. */
typedef struct
{
    char ssid[WT_WIFI_SSID_LEN];
    char passwd[WT_WIFI_PASS_LEN];
} wt_wifi_profile_t;

/** Snapshot of current WiFi state (returned by value, thread-safe). */
typedef struct
{
    /* STA */
    bool sta_started;
    bool sta_connected;
    char sta_ip[20];
    char sta_ssid[WT_WIFI_SSID_LEN];
    int8_t sta_rssi;
    uint8_t sta_active_profile; /*!< Index into profile list         */

    /* AP */
    bool ap_active;
    char ap_ssid[WT_WIFI_SSID_LEN];
    char ap_ip[20];
    uint8_t ap_clients; /*!< Connected client count          */
} wt_wifi_status_t;

/* ------------------------------------------------------------------ */
/*  Task entry point                                                     */
/* ------------------------------------------------------------------ */

/** FreeRTOS task — pin to Core 0. */
void wt_task_wifi(void *pvParameters);

/* ------------------------------------------------------------------ */
/*  Thread-safe management API (callable from any task)                  */
/* ------------------------------------------------------------------ */

/**
 * @brief  Get a snapshot of current WiFi state.
 */
wt_wifi_status_t wt_wifi_get_status(void);

/**
 * @brief  Copy the stored profile list into caller-supplied array.
 * @param  out        Destination array of at least max_count entries.
 * @param  max_count  Maximum profiles to copy.
 * @return            Number of profiles copied.
 */
int wt_wifi_get_profiles(wt_wifi_profile_t *out, int max_count);

/**
 * @brief  Add a new STA profile (persisted to NVS).
 * @return true on success, false if the list is full or args are invalid.
 */
bool wt_wifi_add_profile(const char *ssid, const char *passwd);

/**
 * @brief  Remove profile at index (persisted to NVS).
 * @return true on success.
 */
bool wt_wifi_remove_profile(int index);

/**
 * @brief  Immediately connect to the profile at index.
 *         Disconnects any current session first.
 * @return true if the request was accepted (async — connection may fail).
 */
bool wt_wifi_connect_profile(int index);

#endif /* WT_APP_WIFI_H */
