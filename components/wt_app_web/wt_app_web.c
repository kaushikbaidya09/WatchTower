/*!
    \file   wt_app_web.c
    \brief  ESP-IDF HTTP server — AP-hosted management interface.
 */
#include "wt_app_web.h"
#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_settings.h"
#include "wt_seg_display.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "cJSON.h"

/* ------------------------------------------------------------------ */
/*  Helpers                                                              */
/* ------------------------------------------------------------------ */

#define RESP_JSON(req, json_str)                                       \
    do                                                                 \
    {                                                                  \
        httpd_resp_set_type((req), "application/json");                \
        httpd_resp_set_hdr((req), "Access-Control-Allow-Origin", "*"); \
        httpd_resp_sendstr((req), (json_str));                         \
    } while (0)

#define RESP_ERR(req, msg)                                    \
    do                                                        \
    {                                                         \
        httpd_resp_set_type((req), "application/json");       \
        httpd_resp_set_status((req), "400 Bad Request");      \
        httpd_resp_sendstr((req), "{\"error\":\"" msg "\"}"); \
    } while (0)

typedef esp_err_t (*stream_write_cb_t)(void *ctx, const char *data, size_t len);

typedef struct
{
    bool multipart;
    bool headers_skipped;
    bool done;
    char boundary[80];
    size_t boundary_len;
    char pending[1536];
    size_t pending_len;
} upload_stream_t;

static const char *find_bytes(const char *buf, size_t buf_len,
                              const char *needle, size_t needle_len)
{
    if (!buf || !needle || needle_len == 0 || buf_len < needle_len)
        return NULL;

    for (size_t i = 0; i <= buf_len - needle_len; ++i)
    {
        if (memcmp(buf + i, needle, needle_len) == 0)
            return buf + i;
    }
    return NULL;
}

static bool upload_stream_init(httpd_req_t *req, upload_stream_t *st)
{
    memset(st, 0, sizeof(*st));

    char content_type[128];
    if (httpd_req_get_hdr_value_str(req, "Content-Type",
                                    content_type, sizeof(content_type)) != ESP_OK)
    {
        return true;
    }

    const char *needle = "boundary=";
    char *boundary = strstr(content_type, needle);
    if (!boundary)
    {
        return true;
    }

    boundary += strlen(needle);
    size_t raw_len = strcspn(boundary, ";");
    if (raw_len == 0 || (raw_len + 2) >= sizeof(st->boundary))
    {
        APPLOG_E("Upload boundary too long");
        return false;
    }

    st->boundary[0] = '-';
    st->boundary[1] = '-';
    memcpy(st->boundary + 2, boundary, raw_len);
    st->boundary[raw_len + 2] = '\0';
    st->boundary_len = raw_len + 2;
    st->multipart = true;
    return true;
}

static esp_err_t upload_stream_flush_payload(upload_stream_t *st,
                                             stream_write_cb_t write_cb,
                                             void *ctx,
                                             bool final_flush)
{
    if (!st->multipart)
        return ESP_OK;

    char tail_marker[96];
    size_t tail_len = 2 + st->boundary_len;
    if (tail_len > sizeof(tail_marker))
        return ESP_FAIL;

    tail_marker[0] = '\r';
    tail_marker[1] = '\n';
    memcpy(tail_marker + 2, st->boundary, st->boundary_len);

    const size_t keep_len = tail_len + 4;

    if (final_flush)
    {
        const char *tail = find_bytes(st->pending, st->pending_len,
                                      tail_marker, tail_len);
        if (!tail)
        {
            APPLOG_E("Upload tail boundary not found");
            return ESP_FAIL;
        }

        size_t payload_len = (size_t)(tail - st->pending);
        if (payload_len > 0 && write_cb(ctx, st->pending, payload_len) != ESP_OK)
            return ESP_FAIL;

        st->pending_len = 0;
        st->done = true;
        return ESP_OK;
    }

    if (st->pending_len <= keep_len)
        return ESP_OK;

    size_t flush_len = st->pending_len - keep_len;
    if (write_cb(ctx, st->pending, flush_len) != ESP_OK)
        return ESP_FAIL;

    memmove(st->pending, st->pending + flush_len, keep_len);
    st->pending_len = keep_len;
    return ESP_OK;
}

static esp_err_t upload_stream_consume(upload_stream_t *st,
                                       const char *chunk,
                                       size_t chunk_len,
                                       stream_write_cb_t write_cb,
                                       void *ctx)
{
    if (!st->multipart)
        return write_cb(ctx, chunk, chunk_len);

    if ((st->pending_len + chunk_len) > sizeof(st->pending))
    {
        APPLOG_E("Upload parser buffer overflow");
        return ESP_FAIL;
    }

    memcpy(st->pending + st->pending_len, chunk, chunk_len);
    st->pending_len += chunk_len;

    if (!st->headers_skipped)
    {
        static const char sep[] = "\r\n\r\n";
        const char *hdr_end = find_bytes(st->pending, st->pending_len,
                                         sep, sizeof(sep) - 1);
        if (!hdr_end)
            return ESP_OK;

        size_t offset = (size_t)(hdr_end - st->pending) + (sizeof(sep) - 1);
        memmove(st->pending, st->pending + offset, st->pending_len - offset);
        st->pending_len -= offset;
        st->headers_skipped = true;
    }

    return upload_stream_flush_payload(st, write_cb, ctx, false);
}

static esp_err_t upload_stream_finish(upload_stream_t *st,
                                      stream_write_cb_t write_cb,
                                      void *ctx)
{
    if (!st->multipart)
        return ESP_OK;
    if (!st->headers_skipped)
    {
        APPLOG_E("Upload part headers missing");
        return ESP_FAIL;
    }
    return upload_stream_flush_payload(st, write_cb, ctx, true);
}

/* Read entire request body (up to max_len).  Returns bytes read or -1. */
static int read_body(httpd_req_t *req, char *buf, size_t max_len)
{
    int total = 0;
    int remaining = req->content_len;
    if (remaining <= 0)
        return 0;

    while (remaining > 0)
    {
        int to_read = (remaining < (int)(max_len - total - 1))
                          ? remaining
                          : (int)(max_len - total - 1);
        int n = httpd_req_recv(req, buf + total, to_read);
        if (n <= 0)
        {
            if (n == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            return -1;
        }
        total += n;
        remaining -= n;
    }
    buf[total] = '\0';
    return total;
}

/* Query-string helper */
static bool get_query_int(httpd_req_t *req, const char *key, int *out)
{
    char qbuf[64];
    char val[16];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) != ESP_OK)
        return false;
    if (httpd_query_key_value(qbuf, key, val, sizeof(val)) != ESP_OK)
        return false;
    *out = atoi(val);
    return true;
}

/* ------------------------------------------------------------------ */
/*  SPIFFS                                                               */
/* ------------------------------------------------------------------ */

#define SPIFFS_BASE "/spiffs"
#define INDEX_HTML SPIFFS_BASE "/index.html"
#define STYLE_CSS SPIFFS_BASE "/style.css"
#define APP_JS SPIFFS_BASE "/app.js"

static bool spiffs_mounted = false;

static void mount_spiffs(void)
{
    if (spiffs_mounted)
        return;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_BASE,
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = true,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE)
    {
        spiffs_mounted = true;
        APPLOG_I("SPIFFS mounted");
    }
    else
    {
        APPLOG_E("SPIFFS mount failed: %d", ret);
    }
}

/* Serve a file from SPIFFS using chunked transfer */
static esp_err_t serve_file(httpd_req_t *req, const char *path,
                            const char *content_type)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, content_type);
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
    {
        if (httpd_resp_send_chunk(req, chunk, (ssize_t)n) != ESP_OK)
            break;
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /                                                                */
/* ------------------------------------------------------------------ */

static esp_err_t handler_root(httpd_req_t *req)
{
    struct stat st;
    if (stat(INDEX_HTML, &st) == 0)
    {
        return serve_file(req, INDEX_HTML, "text/html");
    }
    /* Fallback — minimal redirect page */
    const char *fb = "<html><body><h2>WatchTower</h2>"
                     "<p>Upload index.html via OTA tab.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, fb);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /style.css                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handler_style_css(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    return serve_file(req, STYLE_CSS, "text/css");
}

/* ------------------------------------------------------------------ */
/*  GET /app.js                                                          */
/* ------------------------------------------------------------------ */

static esp_err_t handler_app_js(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    return serve_file(req, APP_JS, "application/javascript");
}

/* ------------------------------------------------------------------ */
/*  GET /api/system                                                      */
/* ------------------------------------------------------------------ */

static esp_err_t handler_system(httpd_req_t *req)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t total_heap = esp_get_minimum_free_heap_size() + free_heap; /* approx */
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    /* OTA partition info */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *app0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *app1 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    char ota_slot[8] = "app0";
    char app0_st[12] = "valid";
    char app1_st[12] = "empty";
    if (running && app1 && running->address == app1->address)
    {
        strlcpy(ota_slot, "app1", sizeof(ota_slot));
    }
    (void)app0;
    (void)app1_st;

    /* SPIFFS usage */
    size_t spiffs_total = 0, spiffs_used = 0;
    esp_spiffs_info("spiffs", &spiffs_total, &spiffs_used);
    int spiffs_pct = spiffs_total ? (int)(spiffs_used * 100 / spiffs_total) : 0;

    /* WiFi status for dashboard */
    wt_wifi_status_t wst = wt_wifi_get_status();

    /* Reset reason */
    const char *reset_reason = "unknown";
    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:
        reset_reason = "power-on";
        break;
    case ESP_RST_SW:
        reset_reason = "software";
        break;
    case ESP_RST_PANIC:
        reset_reason = "panic";
        break;
    case ESP_RST_WDT:
        reset_reason = "watchdog";
        break;
    case ESP_RST_DEEPSLEEP:
        reset_reason = "deep-sleep";
        break;
    default:
        break;
    }

    const char *chip_name =
        (chip.model == CHIP_ESP32) ? "ESP32" : (chip.model == CHIP_ESP32S2) ? "ESP32-S2"
                                           : (chip.model == CHIP_ESP32S3)   ? "ESP32-S3"
                                           : (chip.model == CHIP_ESP32C3)   ? "ESP32-C3"
                                                                            : "Unknown";

    /* Build a flat JSON object with all fields the web UI needs */
    static char buf[1024];
    snprintf(buf, sizeof(buf),
             "{"
             "\"chip_model\":\"%s\","
             "\"cpu_cores\":%d,"
             "\"cpu_freq_mhz\":240,"
             "\"cpu_usage\":0,"
             "\"flash_size\":%u,"
             "\"flash_used_pct\":0,"
             "\"free_heap\":%lu,"
             "\"total_heap\":%lu,"
             "\"min_free_heap\":%lu,"
             "\"spiffs_used_pct\":%d,"
             "\"uptime_s\":%lld,"
             "\"app_version\":\"%s\","
             "\"build_date\":\"%s %s\","
             "\"idf_version\":\"%s\","
             "\"ota_slot\":\"%s\","
             "\"app0_state\":\"%s\","
             "\"app1_state\":\"%s\","
             "\"temperature\":0.0,"
             "\"reset_reason\":\"%s\","
             "\"sta_connected\":%s,"
             "\"sta_ssid\":\"%s\","
             "\"sta_ip\":\"%s\","
             "\"rssi\":%d,"
             "\"ap_ip\":\"%s\""
             "}",
             chip_name,
             chip.cores,
             (unsigned)(flash_size),
             free_heap, total_heap, min_heap,
             spiffs_pct,
             (long long)uptime_s,
             "1.0.0",
             __DATE__, __TIME__,
             esp_get_idf_version(),
             ota_slot, app0_st, "empty",
             reset_reason,
             wst.sta_connected ? "true" : "false",
             wst.sta_ssid,
             wst.sta_ip,
             wst.sta_rssi,
             wst.ap_ip);

    RESP_JSON(req, buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/logs?seq=N                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handler_logs(httpd_req_t *req)
{
    int seq_in = 0;
    /* Accept both ?seq= (native) and ?since= (JS legacy) */
    if (!get_query_int(req, "seq", &seq_in))
        get_query_int(req, "since", &seq_in);

    static char log_buf[8192];
    uint32_t next_seq = 0;
    wt_log_read_json((uint32_t)seq_in, log_buf, sizeof(log_buf), &next_seq);
    RESP_JSON(req, log_buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/wifi                                                        */
/* ------------------------------------------------------------------ */

static esp_err_t handler_wifi_get(httpd_req_t *req)
{
    wt_wifi_status_t st = wt_wifi_get_status();
    wt_wifi_profile_t prof[WT_WIFI_MAX_PROFILES];
    int cnt = wt_wifi_get_profiles(prof, WT_WIFI_MAX_PROFILES);

    /* Return a unified flat object; JS reads both connected status and profiles */
    cJSON *root = cJSON_CreateObject();

    /* Flat STA fields (for /api/wifi/status) */
    cJSON_AddBoolToObject(root, "connected", st.sta_connected);
    cJSON_AddStringToObject(root, "ssid", st.sta_ssid);
    cJSON_AddStringToObject(root, "ip", st.sta_ip);
    cJSON_AddNumberToObject(root, "rssi", st.sta_rssi);
    cJSON_AddNumberToObject(root, "channel", 0);
    cJSON_AddStringToObject(root, "ap_ip", st.ap_ip);
    cJSON_AddBoolToObject(root, "ap_active", st.ap_active);
    cJSON_AddStringToObject(root, "ap_ssid", st.ap_ssid);
    cJSON_AddNumberToObject(root, "ap_clients", st.ap_clients);

    /* Profiles array (for /api/wifi/profiles) */
    cJSON *profiles = cJSON_AddArrayToObject(root, "profiles");
    for (int i = 0; i < cnt; i++)
    {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "idx", i);
        cJSON_AddStringToObject(p, "ssid", prof[i].ssid);
        cJSON_AddBoolToObject(p, "has_pass", strlen(prof[i].passwd) > 0);
        cJSON_AddItemToArray(profiles, p);
    }

    char *s = cJSON_PrintUnformatted(root);
    RESP_JSON(req, s);
    free(s);
    cJSON_Delete(root);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/wifi/profile  — add                                        */
/* ------------------------------------------------------------------ */

static esp_err_t handler_wifi_add_profile(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0)
    {
        RESP_ERR(req, "read failed");
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j)
    {
        RESP_ERR(req, "json parse error");
        return ESP_OK;
    }

    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));

    /* Accept both "password" (JS) and "passwd" (legacy) */
    cJSON *passj = cJSON_GetObjectItem(j, "password");
    if (!passj)
        passj = cJSON_GetObjectItem(j, "passwd");
    const char *passwd = passj ? cJSON_GetStringValue(passj) : "";

    bool ok = ssid && wt_wifi_add_profile(ssid, passwd ? passwd : "");
    cJSON_Delete(j);

    RESP_JSON(req, ok ? "{\"status\":\"ok\"}" : "{\"status\":\"err\",\"error\":\"full or invalid\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  DELETE /api/wifi/profile?idx=N                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handler_wifi_del_profile(httpd_req_t *req)
{
    int idx = -1;

    /* Try query param first (DELETE requests) */
    get_query_int(req, "idx", &idx);

    /* Also try JSON body (POST requests from JS) */
    if (idx < 0 && req->content_len > 0)
    {
        char body[128];
        if (read_body(req, body, sizeof(body)) > 0)
        {
            cJSON *j = cJSON_Parse(body);
            if (j)
            {
                cJSON *idxj = cJSON_GetObjectItem(j, "index");
                if (!idxj)
                    idxj = cJSON_GetObjectItem(j, "idx");
                if (idxj)
                    idx = (int)cJSON_GetNumberValue(idxj);
                cJSON_Delete(j);
            }
        }
    }

    if (idx < 0)
    {
        RESP_ERR(req, "missing idx");
        return ESP_OK;
    }

    bool ok = wt_wifi_remove_profile(idx);
    RESP_JSON(req, ok ? "{\"status\":\"ok\"}" : "{\"status\":\"err\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/wifi/connect?idx=N                                         */
/* ------------------------------------------------------------------ */

static esp_err_t handler_wifi_connect(httpd_req_t *req)
{
    int idx = -1;
    get_query_int(req, "idx", &idx);
    if (idx < 0 && req->content_len > 0)
    {
        char body[128];
        if (read_body(req, body, sizeof(body)) > 0)
        {
            cJSON *j = cJSON_Parse(body);
            if (j)
            {
                cJSON *idxj = cJSON_GetObjectItem(j, "index");
                if (!idxj)
                    idxj = cJSON_GetObjectItem(j, "idx");
                if (idxj)
                    idx = (int)cJSON_GetNumberValue(idxj);
                cJSON_Delete(j);
            }
        }
    }
    if (idx < 0)
    {
        RESP_ERR(req, "missing idx");
        return ESP_OK;
    }
    bool ok = wt_wifi_connect_profile(idx);
    RESP_JSON(req, ok ? "{\"status\":\"ok\"}" : "{\"status\":\"err\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/settings                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_settings_get(httpd_req_t *req)
{
    wt_settings_t s = wt_settings_get();

    /* brightness mapped 0-100 from intensity 0-255 */
    int brt = (int)((s.intensity * 100) / 255);

    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{"
             "\"color\":\"%s\","
             "\"brightness\":%d,"
             "\"anim_colon\":%s,"
             "\"anim_scroll\":%s,"
             "\"anim_pulse\":%s,"
             "\"anim_transition\":%s,"
             "\"reaction_effect\":\"%s\","
             "\"time_format\":%d,"
             "\"timezone\":\"%s\","
             "\"ntp_server\":\"%s\","
             "\"alarm1_time\":\"%s\","
             "\"alarm1_en\":%s,"
             "\"alarm2_time\":\"%s\","
             "\"alarm2_en\":%s,"
             "\"notif_type\":\"%s\","
             "\"notif_sound\":\"%s\","
             "\"sleep_mode\":\"%s\","
             "\"sleep_timeout\":%d,"
             "\"batt_alert_pct\":%d,"
             "\"ps_dim\":%s,"
             "\"ps_wifi\":%s"
             "}",
             s.color_hex[0] ? s.color_hex : "#00c8ff",
             brt,
             s.colon_blink ? "true" : "false",
             s.anim_scroll ? "true" : "false",
             s.anim_pulse ? "true" : "false",
             s.anim_transition ? "true" : "false",
             s.reaction_effect,
             s.time_format,
             s.timezone,
             s.ntp_server,
             s.alarm1_time,
             s.alarm1_en ? "true" : "false",
             s.alarm2_time,
             s.alarm2_en ? "true" : "false",
             s.notif_type,
             s.notif_sound,
             s.sleep_mode,
             s.sleep_timeout_s,
             s.batt_alert_pct,
             s.ps_dim ? "true" : "false",
             s.ps_wifi ? "true" : "false");
    RESP_JSON(req, buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/settings                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handler_settings_set(httpd_req_t *req)
{
    char body[1024];
    if (read_body(req, body, sizeof(body)) < 0)
    {
        RESP_ERR(req, "read failed");
        return ESP_OK;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j)
    {
        RESP_ERR(req, "json error");
        return ESP_OK;
    }

    wt_settings_t s = wt_settings_get();

    /* ── Display fields ── */
    cJSON *color = cJSON_GetObjectItem(j, "color");
    if (color && cJSON_IsString(color))
    {
        const char *hex = cJSON_GetStringValue(color);
        strlcpy(s.color_hex, hex, sizeof(s.color_hex));
        wt_settings_parse_hex_color(hex,
                                    &s.color_on.r, &s.color_on.g, &s.color_on.b);
    }
    cJSON *brt = cJSON_GetObjectItem(j, "brightness");
    if (brt)
    {
        int pct = (int)cJSON_GetNumberValue(brt);
        s.intensity = (uint8_t)((pct * 255) / 100);
    }
    cJSON *cb = cJSON_GetObjectItem(j, "anim_colon");
    if (cb)
        s.colon_blink = cJSON_IsTrue(cb);
    cJSON *asc = cJSON_GetObjectItem(j, "anim_scroll");
    if (asc)
        s.anim_scroll = cJSON_IsTrue(asc);
    cJSON *apu = cJSON_GetObjectItem(j, "anim_pulse");
    if (apu)
        s.anim_pulse = cJSON_IsTrue(apu);
    cJSON *atr = cJSON_GetObjectItem(j, "anim_transition");
    if (atr)
        s.anim_transition = cJSON_IsTrue(atr);
    cJSON *reff = cJSON_GetObjectItem(j, "reaction_effect");
    if (reff && cJSON_IsString(reff))
        strlcpy(s.reaction_effect, cJSON_GetStringValue(reff), sizeof(s.reaction_effect));

    /* ── Clock fields ── */
    cJSON *tf = cJSON_GetObjectItem(j, "time_format");
    if (tf)
        s.time_format = (uint8_t)cJSON_GetNumberValue(tf);
    cJSON *tz = cJSON_GetObjectItem(j, "timezone");
    if (tz && cJSON_IsString(tz))
        strlcpy(s.timezone, cJSON_GetStringValue(tz), sizeof(s.timezone));
    cJSON *ntp = cJSON_GetObjectItem(j, "ntp_server");
    if (ntp && cJSON_IsString(ntp))
        strlcpy(s.ntp_server, cJSON_GetStringValue(ntp), sizeof(s.ntp_server));
    cJSON *al1t = cJSON_GetObjectItem(j, "alarm1_time");
    if (al1t && cJSON_IsString(al1t))
        strlcpy(s.alarm1_time, cJSON_GetStringValue(al1t), sizeof(s.alarm1_time));
    cJSON *al1e = cJSON_GetObjectItem(j, "alarm1_en");
    if (al1e)
        s.alarm1_en = cJSON_IsTrue(al1e);
    cJSON *al2t = cJSON_GetObjectItem(j, "alarm2_time");
    if (al2t && cJSON_IsString(al2t))
        strlcpy(s.alarm2_time, cJSON_GetStringValue(al2t), sizeof(s.alarm2_time));
    cJSON *al2e = cJSON_GetObjectItem(j, "alarm2_en");
    if (al2e)
        s.alarm2_en = cJSON_IsTrue(al2e);
    cJSON *ntt = cJSON_GetObjectItem(j, "notif_type");
    if (ntt && cJSON_IsString(ntt))
        strlcpy(s.notif_type, cJSON_GetStringValue(ntt), sizeof(s.notif_type));
    cJSON *nts = cJSON_GetObjectItem(j, "notif_sound");
    if (nts && cJSON_IsString(nts))
        strlcpy(s.notif_sound, cJSON_GetStringValue(nts), sizeof(s.notif_sound));

    /* ── Power fields ── */
    cJSON *slm = cJSON_GetObjectItem(j, "sleep_mode");
    if (slm && cJSON_IsString(slm))
        strlcpy(s.sleep_mode, cJSON_GetStringValue(slm), sizeof(s.sleep_mode));
    cJSON *slt = cJSON_GetObjectItem(j, "sleep_timeout");
    if (slt)
        s.sleep_timeout_s = (uint16_t)cJSON_GetNumberValue(slt);
    cJSON *bat = cJSON_GetObjectItem(j, "batt_alert_pct");
    if (bat)
        s.batt_alert_pct = (uint8_t)cJSON_GetNumberValue(bat);
    cJSON *psd = cJSON_GetObjectItem(j, "ps_dim");
    if (psd)
        s.ps_dim = cJSON_IsTrue(psd);
    cJSON *psw = cJSON_GetObjectItem(j, "ps_wifi");
    if (psw)
        s.ps_wifi = cJSON_IsTrue(psw);

    cJSON_Delete(j);

    bool ok = wt_settings_set(&s);
    RESP_JSON(req, ok ? "{\"status\":\"ok\"}" : "{\"status\":\"err\",\"error\":\"nvs write failed\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/power                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handler_power_get(httpd_req_t *req)
{
    wt_settings_t s = wt_settings_get();
    /* Battery reading — extend wt_app_hw.h for real ADC values */
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{"
             "\"battery_pct\":0,"
             "\"voltage\":0.0,"
             "\"current\":0,"
             "\"source\":\"USB\","
             "\"eta_hours\":0,"
             "\"sleep_mode\":\"%s\","
             "\"sleep_timeout\":%d,"
             "\"batt_alert_pct\":%d,"
             "\"ps_dim\":%s,"
             "\"ps_wifi\":%s"
             "}",
             s.sleep_mode, s.sleep_timeout_s, s.batt_alert_pct,
             s.ps_dim ? "true" : "false",
             s.ps_wifi ? "true" : "false");
    RESP_JSON(req, buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ntp/sync                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handler_ntp_sync(httpd_req_t *req)
{
    char body[256];
    read_body(req, body, sizeof(body));
    cJSON *j = cJSON_Parse(body);
    if (j)
    {
        cJSON *srv = cJSON_GetObjectItem(j, "server");
        if (srv && cJSON_IsString(srv))
        {
            wt_settings_t s = wt_settings_get();
            strlcpy(s.ntp_server, cJSON_GetStringValue(srv),
                    sizeof(s.ntp_server));
            wt_settings_set(&s);
        }
        cJSON_Delete(j);
    }
    /* Trigger SNTP resync if available */
    APPLOG_I("NTP sync requested");
    RESP_JSON(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}

static esp_err_t ota_write_cb(void *ctx, const char *data, size_t len)
{
    esp_ota_handle_t ota_handle = *(esp_ota_handle_t *)ctx;
    return esp_ota_write(ota_handle, data, len);
}

static esp_err_t file_write_cb(void *ctx, const char *data, size_t len)
{
    FILE *f = (FILE *)ctx;
    return (fwrite(data, 1, len, f) == len) ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ota/firmware  — OTA firmware update                        */
/* ------------------------------------------------------------------ */

static esp_err_t handler_ota_firmware(httpd_req_t *req)
{
    APPLOG_I("OTA firmware: %d bytes incoming", req->content_len);

    const esp_partition_t *update_part =
        esp_ota_get_next_update_partition(NULL);
    if (!update_part)
    {
        RESP_ERR(req, "no OTA partition");
        return ESP_OK;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part,
                                  OTA_WITH_SEQUENTIAL_WRITES,
                                  &ota_handle);
    if (err != ESP_OK)
    {
        RESP_ERR(req, "ota_begin failed");
        return ESP_OK;
    }

    upload_stream_t stream;
    if (!upload_stream_init(req, &stream))
    {
        esp_ota_abort(ota_handle);
        RESP_ERR(req, "invalid upload content-type");
        return ESP_OK;
    }

    char buf[1024];
    int remaining = req->content_len;
    bool ota_ok = true;
    bool wrote_bytes = false;

    while (remaining > 0)
    {
        int to_recv = (remaining < (int)sizeof(buf))
                          ? remaining
                          : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, to_recv);
        if (n <= 0)
        {
            if (n == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            ota_ok = false;
            break;
        }
        if (upload_stream_consume(&stream, buf, (size_t)n, ota_write_cb, &ota_handle) != ESP_OK)
        {
            ota_ok = false;
            break;
        }
        wrote_bytes = true;
        remaining -= n;
    }

    if (ota_ok && upload_stream_finish(&stream, ota_write_cb, &ota_handle) != ESP_OK)
        ota_ok = false;

    if (!ota_ok || !wrote_bytes)
    {
        esp_ota_abort(ota_handle);
        RESP_ERR(req, "ota write/end failed");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK)
    {
        APPLOG_E("esp_ota_end failed: %s", esp_err_to_name(err));
        RESP_ERR(req, "ota write/end failed");
        return ESP_OK;
    }

    if (esp_ota_set_boot_partition(update_part) != ESP_OK)
    {
        RESP_ERR(req, "set boot partition failed");
        return ESP_OK;
    }

    APPLOG_I("OTA firmware done — rebooting in 2 s");
    RESP_JSON(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ota/webapp  — update SPIFFS index.html                     */
/* ------------------------------------------------------------------ */

static esp_err_t handler_ota_webapp(httpd_req_t *req)
{
    APPLOG_I("WebApp OTA: %d bytes incoming", req->content_len);

    FILE *f = fopen(INDEX_HTML, "w");
    if (!f)
    {
        RESP_ERR(req, "cannot open spiffs file");
        return ESP_OK;
    }

    upload_stream_t stream;
    if (!upload_stream_init(req, &stream))
    {
        fclose(f);
        RESP_ERR(req, "invalid upload content-type");
        return ESP_OK;
    }

    char buf[512];
    int remaining = req->content_len;
    bool ok = true;

    while (remaining > 0)
    {
        int to_recv = (remaining < (int)sizeof(buf))
                          ? remaining
                          : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, to_recv);
        if (n <= 0)
        {
            if (n == HTTPD_SOCK_ERR_TIMEOUT)
                continue;
            ok = false;
            break;
        }
        if (upload_stream_consume(&stream, buf, (size_t)n, file_write_cb, f) != ESP_OK)
        {
            ok = false;
            break;
        }
        remaining -= n;
    }

    if (ok && upload_stream_finish(&stream, file_write_cb, f) != ESP_OK)
        ok = false;

    fclose(f);

    if (ok)
    {
        APPLOG_I("WebApp OTA complete");
        RESP_JSON(req, "{\"ok\":true}");
    }
    else
    {
        APPLOG_E("WebApp OTA write error");
        RESP_JSON(req, "{\"ok\":false,\"error\":\"write error\"}");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/reboot                                                     */
/* ------------------------------------------------------------------ */

static esp_err_t handler_reboot(httpd_req_t *req)
{
    RESP_JSON(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  Server start                                                         */
/* ------------------------------------------------------------------ */

static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 28;
    cfg.stack_size = 16384;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        APPLOG_E("Failed to start HTTP server");
        return;
    }

#define REG(m, u, h)                                                  \
    do                                                                \
    {                                                                 \
        httpd_uri_t _u = {.uri = (u), .method = (m), .handler = (h)}; \
        httpd_register_uri_handler(server, &_u);                      \
    } while (0)

    REG(HTTP_GET, "/", handler_root);
    REG(HTTP_GET, "/style.css", handler_style_css);
    REG(HTTP_GET, "/app.js", handler_app_js);
    REG(HTTP_GET, "/api/system", handler_system);
    REG(HTTP_GET, "/api/status", handler_system);
    REG(HTTP_GET, "/api/logs", handler_logs);
    REG(HTTP_GET, "/api/wifi", handler_wifi_get);
    REG(HTTP_GET, "/api/wifi/status", handler_wifi_get);
    REG(HTTP_GET, "/api/wifi/profiles", handler_wifi_get);
    REG(HTTP_POST, "/api/wifi/profile", handler_wifi_add_profile);
    REG(HTTP_POST, "/api/wifi/profiles", handler_wifi_add_profile);
    REG(HTTP_DELETE, "/api/wifi/profile", handler_wifi_del_profile);
    REG(HTTP_POST, "/api/wifi/profile/delete", handler_wifi_del_profile);
    REG(HTTP_POST, "/api/wifi/connect", handler_wifi_connect);
    REG(HTTP_GET, "/api/settings", handler_settings_get);
    REG(HTTP_POST, "/api/settings", handler_settings_set);
    REG(HTTP_POST, "/api/clock", handler_settings_set);
    REG(HTTP_POST, "/api/power", handler_settings_set);
    REG(HTTP_GET, "/api/power", handler_power_get);
    REG(HTTP_POST, "/api/ntp/sync", handler_ntp_sync);
    REG(HTTP_POST, "/api/ota/firmware", handler_ota_firmware);
    REG(HTTP_POST, "/api/ota/webapp", handler_ota_webapp);
    REG(HTTP_POST, "/api/reboot", handler_reboot);

#undef REG

    APPLOG_I("HTTP server started on port 80");
}

/* ------------------------------------------------------------------ */
/*  Task                                                                 */
/* ------------------------------------------------------------------ */

void wt_task_web(void *pvParameters)
{
    // APPLOG_I("---------- WEB TASK STARTED ----------");

    /* Wait for WiFi AP to come up */
    vTaskDelay(pdMS_TO_TICKS(3000));

    mount_spiffs();
    start_server();

    /* Task keeps running — httpd uses its own internal task */
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}
