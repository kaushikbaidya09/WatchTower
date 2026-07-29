/*!
    \file   wt_app_web.c
    \brief  ESP-IDF HTTP server, AP-hosted management interface.

    \details
    See wt_app_web.h for the HTTP/WebSocket endpoint reference.
 */

#include "wt_app_web.h"
#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_settings.h"
#include "wt_seg_display.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "driver/temperature_sensor.h"
#include "cJSON.h"

/* Captive-portal DNS server */
#include "lwip/sockets.h"
#include "lwip/netdb.h"

/* No wildcard CORS: every RESP_JSON caller is a state-changing OTA endpoint
   (see finding #7) and same-origin requests from the served SPA don't need it. */
#define RESP_JSON(req, json_str)                          \
    do                                                    \
    {                                                     \
        httpd_resp_set_type((req), "application/json");   \
        httpd_resp_sendstr((req), (json_str));             \
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

static temperature_sensor_handle_t s_temp_sensor = NULL;
static bool s_temp_sensor_ready = false;

#define WS_MAX_CLIENTS 4          ///< Maximum allowed clients
#define WS_MAX_INBOUND_PAYLOAD 8192 ///< Cap on an incoming WS frame before it is malloc'd (DoS guard)
#define WS_DISPLAY_INTERVAL_MS 50 ///< Web Display fetch refresh rate
#define WS_FULL_INTERVAL_MS 2000  ///< Full system updates rate (system/wifi/power/logs)
#define WS_DISPLAY_PAYLOAD 1536   ///< display-only frame ~700 B
#define WS_PAYLOAD_SIZE 12288     ///< full frame
#define WS_FULL_TICKS (WS_FULL_INTERVAL_MS / WS_DISPLAY_INTERVAL_MS)

static httpd_handle_t s_http_server = NULL;
static int s_ws_fds[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;

/* Set whenever a client (re)connects; tells the next tier-2/3 tick to send
   settings/wifi/power in full instead of "unchanged" (null), so a freshly
   connected client always gets an initial snapshot regardless of whether
   anything has actually changed since the last broadcast. */
static volatile bool s_ws_new_client = false;

/*!
    \brief  Marks every WS client slot as free (-1).
 */
static void ws_clients_init(void)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        s_ws_fds[i] = -1;
}

/*!
    \brief  Registers a newly connected WS client fd in the client table
            (a no-op if already present) and flags that a new client
            connected so the next full frame gives it an initial snapshot.
 */
static void ws_client_add(int fd)
{
    if (!s_ws_mutex || xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return;

    bool already = false;
    int free_slot = -1;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (s_ws_fds[i] == fd)
        {
            already = true;
            break;
        }
        if (free_slot < 0 && s_ws_fds[i] < 0)
            free_slot = i;
    }
    if (!already && free_slot >= 0)
        s_ws_fds[free_slot] = fd;
    xSemaphoreGive(s_ws_mutex);

    if (already)
        return;

    s_ws_new_client = true;
    wt_log_info("WS client connected fd=%d", fd);
}

/*!
    \brief  Removes a WS client fd from the client table on disconnect.
 */
static void ws_client_remove(int fd)
{
    if (!s_ws_mutex || xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (s_ws_fds[i] == fd)
        {
            s_ws_fds[i] = -1;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    wt_log_info("WS client disconnected fd=%d", fd);
}

/*!
    \brief  Finds the first occurrence of a byte sequence within a buffer
            (memmem-style search over raw, possibly non-NUL-terminated bytes).
 */
static const char *find_bytes(const char *buf, size_t buf_len, const char *needle, size_t needle_len)
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

/*!
    \brief  Parses the request's Content-Type header to detect a
            multipart/form-data upload and extract its MIME boundary.
 */
static bool upload_stream_init(httpd_req_t *req, upload_stream_t *st)
{
    memset(st, 0, sizeof(*st));

    char content_type[128];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK)
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
        wt_log_error("Upload boundary too long");
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

/*!
    \brief  Flushes buffered multipart body bytes to the write callback,
            holding back enough trailing bytes to still detect the closing
            boundary marker on the next call (or emits the final payload
            up to that marker when \p final_flush is set).
 */
static esp_err_t upload_stream_flush_payload(upload_stream_t *st, stream_write_cb_t write_cb, void *ctx, bool final_flush)
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
            wt_log_error("Upload tail boundary not found");
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

/*!
    \brief  Feeds one received chunk into the upload stream parser,
            skipping the multipart part headers before the first flush.
 */
static esp_err_t upload_stream_consume(upload_stream_t *st, const char *chunk, size_t chunk_len, stream_write_cb_t write_cb, void *ctx)
{
    if (!st->multipart)
        return write_cb(ctx, chunk, chunk_len);

    if ((st->pending_len + chunk_len) > sizeof(st->pending))
    {
        wt_log_error("Upload parser buffer overflow");
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

/*!
    \brief  Flushes the final buffered payload bytes once the upload body
            has been completely received.
 */
static esp_err_t upload_stream_finish(upload_stream_t *st, stream_write_cb_t write_cb, void *ctx)
{
    if (!st->multipart)
        return ESP_OK;

    if (!st->headers_skipped)
    {
        wt_log_error("Upload part headers missing");
        return ESP_FAIL;
    }
    return upload_stream_flush_payload(st, write_cb, ctx, true);
}

/*!
    \brief  Lazily installs and enables the on-chip temperature sensor
            peripheral on first use.
 */
static bool ensure_temp_sensor_ready(void)
{
    if (s_temp_sensor_ready)
        return true;

    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_temp_sensor);
    if (err != ESP_OK)
    {
        wt_log_error("temperature_sensor_install failed: %s", esp_err_to_name(err));
        s_temp_sensor = NULL;
        return false;
    }

    err = temperature_sensor_enable(s_temp_sensor);
    if (err != ESP_OK)
    {
        wt_log_error("temperature_sensor_enable failed: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = NULL;
        return false;
    }

    s_temp_sensor_ready = true;
    return true;
}

/*!
    \brief  Reads the current MCU die temperature in Celsius via the
            temperature sensor peripheral.
 */
static bool read_mcu_temperature_c(float *out_celsius)
{
    if (!out_celsius)
        return false;

    if (!ensure_temp_sensor_ready())
        return false;

    esp_err_t err = temperature_sensor_get_celsius(s_temp_sensor, out_celsius);
    if (err != ESP_OK)
    {
        wt_log_error("temperature_sensor_get_celsius failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

/*!
    \brief  Converts a wt_segd_anim_t enum value to its wire-format string
            name.
 */
static const char *anim_to_string(wt_segd_anim_t anim)
{
    switch (anim)
    {
    case WT_SEGD_ANIM_PULSE:
        return "pulse";
    case WT_SEGD_ANIM_RAINBOW:
        return "rainbow";
    case WT_SEGD_ANIM_WAVE:
        return "wave";
    case WT_SEGD_ANIM_COLOR_FLOW:
        return "colorflow";
    case WT_SEGD_ANIM_SOLID:
    default:
        return "solid";
    }
}

/*!
    \brief  Converts a wt_segd_mode_t enum value to its wire-format string
            name.
 */
static const char *mode_to_string(wt_segd_mode_t mode)
{
    switch (mode)
    {
    case WT_SEGD_MODE_TIME:
        return "time";
    case WT_SEGD_MODE_TEXT:
        return "text";
    case WT_SEGD_MODE_RAW:
        return "raw";
    case WT_SEGD_MODE_NUMBER:
    default:
        return "number";
    }
}

/*!
    \brief  Builds a [red,green,blue] cJSON array from a wt_segd_color_t.
 */
static cJSON *color_to_json(wt_segd_color_t color)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(color.red));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(color.green));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(color.blue));
    return arr;
}

/*!
    \brief  Compares two wt_segd_color_t values for equality.
 */
static bool color_eq(wt_segd_color_t a, wt_segd_color_t b)
{
    return (a.red == b.red) && (a.green == b.green) && (a.blue == b.blue);
}

/*!
    \brief  true if every per-segment color in the snapshot equals color_on,
            i.e. the common "single color for the whole display" case where
            sending the full per-digit-per-segment color array would be
            redundant the client can just fall back to color_on.
 */
static bool digit_colors_all_match_color_on(const wt_segd_snapshot_t *snap)
{
    for (int di = 0; di < WT_SEGD_NUM_DIGITS; di++)
    {
        for (int si = 0; si < WT_SEGD_SEGS_PER_DIGIT; si++)
        {
            if (!color_eq(snap->digit_color[di][si], snap->request.color_on))
            {
                return false;
            }
        }
    }
    return true;
}

/*!
    \brief  Uppercases and filters an input string down to the display's
            supported character set (A-Z, 0-9, space, underscore, hyphen).
 */
static void sanitize_display_text(const char *src, char *dst, size_t dst_len)
{
    if (!dst || dst_len == 0)
    {
        return;
    }

    size_t out = 0;
    while (src && *src && out < (dst_len - 1))
    {
        char ch = *src++;
        if ((ch >= 'a') && (ch <= 'z'))
        {
            ch = (char)(ch - ('a' - 'A'));
        }
        if (((ch >= 'A') && (ch <= 'Z')) ||
            ((ch >= '0') && (ch <= '9')) ||
            (ch == ' ') || (ch == '_') || (ch == '-'))
        {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
}

/* Note: no local "apply settings to display now" helper here wt_settings_set()
   already notifies the app_main display-request loop (see wt_app_settings.c), which rebuilds and
   pushes the wt_segd_request_t from the same wt_settings_t. Duplicating that
   mapping here would just be a second producer racing to write the same
   single-slot queue. */

typedef enum
{
    WT_SF_STR,           ///< Plain string, strlcpy both directions
    WT_SF_STR_SANITIZED, ///< String, sanitized on parse (see sanitize_display_text)
    WT_SF_BOOL,
    WT_SF_U8,
    WT_SF_U16,
    WT_SF_I16,
    WT_SF_U8_PCT255, ///< uint8_t 0-255 struct field <-> wire percent 0-100
    WT_SF_COLOR_HEX, ///< color_hex[] string; parsing also derives color_on RGB
} wt_settings_field_type_t;

typedef struct
{
    const char *key;
    wt_settings_field_type_t type;
    size_t offset; ///< offsetof(wt_settings_t, member)
    size_t size;   ///< sizeof(member) destination buffer size for strings
} wt_settings_field_t;

/* sizeof-of-member below never dereferences the null pointer: sizeof's
   operand is not evaluated (C11 6.5.3.4), so this is the standard,
   allocation-free "size of a struct member without an instance" idiom. */
#define WT_SF(key_, type_, member_) \
    { (key_), (type_), offsetof(wt_settings_t, member_), sizeof(((wt_settings_t *)0)->member_) }

static const wt_settings_field_t s_settings_fields[] = {
    WT_SF("color", WT_SF_COLOR_HEX, color_hex),
    WT_SF("brightness", WT_SF_U8_PCT255, intensity),
    WT_SF("anim_colon", WT_SF_BOOL, colon_blink),
    WT_SF("anim_scroll", WT_SF_BOOL, anim_scroll),
    WT_SF("anim_pulse", WT_SF_BOOL, anim_pulse),
    WT_SF("anim_transition", WT_SF_BOOL, anim_transition),
    WT_SF("reaction_effect", WT_SF_STR, reaction_effect),
    WT_SF("display_mode", WT_SF_STR, display_mode),
    WT_SF("display_value", WT_SF_I16, display_value),
    WT_SF("display_text", WT_SF_STR_SANITIZED, display_text),
    WT_SF("bg_effect_en", WT_SF_BOOL, bg_effect_en),
    WT_SF("anim_effect", WT_SF_STR, anim_effect),
    WT_SF("led_count", WT_SF_U16, led_count),
    WT_SF("time_format", WT_SF_U8, time_format),
    WT_SF("timezone", WT_SF_STR, timezone),
    WT_SF("ntp_server", WT_SF_STR, ntp_server),
    WT_SF("alarm1_time", WT_SF_STR, alarm1_time),
    WT_SF("alarm1_en", WT_SF_BOOL, alarm1_en),
    WT_SF("alarm2_time", WT_SF_STR, alarm2_time),
    WT_SF("alarm2_en", WT_SF_BOOL, alarm2_en),
    WT_SF("notif_type", WT_SF_STR, notif_type),
    WT_SF("notif_sound", WT_SF_STR, notif_sound),
    WT_SF("sleep_mode", WT_SF_STR, sleep_mode),
    WT_SF("sleep_timeout", WT_SF_U16, sleep_timeout_s),
    WT_SF("batt_alert_pct", WT_SF_U8, batt_alert_pct),
    WT_SF("ps_dim", WT_SF_BOOL, ps_dim),
    WT_SF("ps_wifi", WT_SF_BOOL, ps_wifi),
};
#define WT_SETTINGS_FIELD_COUNT (sizeof(s_settings_fields) / sizeof(s_settings_fields[0]))

/*!
    \brief  Looks up a settings wire field descriptor by its JSON key name.
 */
static const wt_settings_field_t *find_settings_field(const char *key)
{
    for (size_t i = 0; i < WT_SETTINGS_FIELD_COUNT; i++)
    {
        if (strcmp(s_settings_fields[i].key, key) == 0)
        {
            return &s_settings_fields[i];
        }
    }
    return NULL;
}

/*!
    \brief  Apply one JSON value onto the matching wt_settings_t member,
            per the field's declared wire type.
 */
static void settings_field_apply(wt_settings_t *s, const wt_settings_field_t *f, cJSON *val)
{
    void *field = (uint8_t *)s + f->offset;

    switch (f->type)
    {
    case WT_SF_STR:
        if (cJSON_IsString(val))
        {
            strlcpy((char *)field, cJSON_GetStringValue(val), f->size);
        }
        break;
    case WT_SF_STR_SANITIZED:
        if (cJSON_IsString(val))
        {
            sanitize_display_text(cJSON_GetStringValue(val), (char *)field, f->size);
        }
        break;
    case WT_SF_BOOL:
        *(bool *)field = cJSON_IsTrue(val);
        break;
    case WT_SF_U8:
        *(uint8_t *)field = (uint8_t)cJSON_GetNumberValue(val);
        break;
    case WT_SF_U16:
        *(uint16_t *)field = (uint16_t)cJSON_GetNumberValue(val);
        break;
    case WT_SF_I16:
        *(int16_t *)field = (int16_t)cJSON_GetNumberValue(val);
        break;
    case WT_SF_U8_PCT255:
        *(uint8_t *)field = (uint8_t)(((int)cJSON_GetNumberValue(val) * 255) / 100);
        break;
    case WT_SF_COLOR_HEX:
        if (cJSON_IsString(val))
        {
            const char *hex = cJSON_GetStringValue(val);
            strlcpy((char *)field, hex, f->size);
            wt_settings_parse_hex_color(hex, &s->color_on.red, &s->color_on.green, &s->color_on.blue);
        }
        break;
    }
}

/*!
    \brief  Merge every wire key present in \p j onto \p s, table-driven.
            Unknown keys and keys not present in \p j are ignored.
 */
static void settings_apply_json(wt_settings_t *s, cJSON *j)
{
    for (size_t i = 0; i < WT_SETTINGS_FIELD_COUNT; i++)
    {
        cJSON *val = cJSON_GetObjectItem(j, s_settings_fields[i].key);
        if (val)
        {
            settings_field_apply(s, &s_settings_fields[i], val);
        }
    }
}

/*!
    \brief  Add one wt_settings_t member onto a cJSON object, per the
            field's declared wire type.
 */
static void settings_field_to_json(cJSON *obj, const wt_settings_t *s, const wt_settings_field_t *f)
{
    const void *field = (const uint8_t *)s + f->offset;

    switch (f->type)
    {
    case WT_SF_STR:
    case WT_SF_STR_SANITIZED:
        cJSON_AddStringToObject(obj, f->key, (const char *)field);
        break;
    case WT_SF_COLOR_HEX:
    {
        const char *hex = *(const char *)field ? (const char *)field : "#00c8ff";
        cJSON_AddStringToObject(obj, f->key, hex);
        break;
    }
    case WT_SF_BOOL:
        cJSON_AddBoolToObject(obj, f->key, *(const bool *)field);
        break;
    case WT_SF_U8:
        cJSON_AddNumberToObject(obj, f->key, *(const uint8_t *)field);
        break;
    case WT_SF_U16:
        cJSON_AddNumberToObject(obj, f->key, *(const uint16_t *)field);
        break;
    case WT_SF_I16:
        cJSON_AddNumberToObject(obj, f->key, *(const int16_t *)field);
        break;
    case WT_SF_U8_PCT255:
        cJSON_AddNumberToObject(obj, f->key, ((int)(*(const uint8_t *)field) * 100) / 255);
        break;
    }
}

/*!
    \brief  Build the full "settings" wire object (every field in the table).
 */
static cJSON *settings_to_json_object(const wt_settings_t *s)
{
    cJSON *obj = cJSON_CreateObject();
    for (size_t i = 0; i < WT_SETTINGS_FIELD_COUNT; i++)
    {
        settings_field_to_json(obj, s, &s_settings_fields[i]);
    }
    return obj;
}

#define SPIFFS_BASE "/spiffs"
#define INDEX_HTML SPIFFS_BASE "/index.html"
#define STYLE_CSS SPIFFS_BASE "/style.css"
#define APP_JS SPIFFS_BASE "/app.js"

static bool spiffs_mounted = false;

/*!
    \brief  Mounts (formatting first if needed) the SPIFFS partition;
            idempotent across repeated calls.
 */
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
        wt_log_info("SPIFFS mounted");
    }
    else
    {
        wt_log_error("SPIFFS mount failed: %d", ret);
    }
}

/*!
    \brief  Serves a file from SPIFFS to the HTTP client using chunked
            transfer, or a 404 if it doesn't exist.
 */
static esp_err_t serve_file(httpd_req_t *req, const char *path, const char *content_type)
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

/*!
    \brief  GET / handler: serves the SPA's index.html from SPIFFS, or a
            minimal fallback placeholder page if it hasn't been uploaded yet.
 */
static esp_err_t handler_root(httpd_req_t *req)
{
    struct stat st;
    if (stat(INDEX_HTML, &st) == 0)
    {
        return serve_file(req, INDEX_HTML, "text/html");
    }
    /* Fallback minimal redirect page */
    const char *fb = "<html><body><h2>WatchTower</h2>"
                     "<p>Upload index.html via OTA tab.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, fb);
    return ESP_OK;
}

/*!
    \brief  GET /style.css handler: serves the SPA stylesheet from SPIFFS
            with a short cache lifetime.
 */
static esp_err_t handler_style_css(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    return serve_file(req, STYLE_CSS, "text/css");
}

/*!
    \brief  GET /app.js handler: serves the SPA script from SPIFFS with a
            short cache lifetime.
 */
static esp_err_t handler_app_js(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    return serve_file(req, APP_JS, "application/javascript");
}

/*!
    \brief  upload_stream_t write callback that appends received bytes to
            an in-progress OTA update.
 */
static esp_err_t ota_write_cb(void *ctx, const char *data, size_t len)
{
    esp_ota_handle_t ota_handle = *(esp_ota_handle_t *)ctx;
    return esp_ota_write(ota_handle, data, len);
}

/*!
    \brief  upload_stream_t write callback that appends received bytes to
            an open SPIFFS file.
 */
static esp_err_t file_write_cb(void *ctx, const char *data, size_t len)
{
    FILE *f = (FILE *)ctx;
    return (fwrite(data, 1, len, f) == len) ? ESP_OK : ESP_FAIL;
}

/*!
    \brief  True if the connected client's peer address is on the AP subnet
            (192.168.4.0/24) rather than the trusted STA network.
            Fails closed (treats "unknown" as AP) if the peer can't be read.
 */
static bool ota_client_is_on_ap_subnet(httpd_req_t *req)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&addr, &len) != 0)
    {
        return true;
    }
    const uint8_t *octet = (const uint8_t *)&addr.sin_addr.s_addr;
    return (octet[0] == 192) && (octet[1] == 168) && (octet[2] == 4);
}

/*!
    \brief  Reject OTA requests arriving from the (untrusted) AP interface.
            No token check anyone on the trusted STA network can push an
            update, same as anyone on that network can use the WS commands.
 */
static bool ota_request_authorized(httpd_req_t *req)
{
    if (ota_client_is_on_ap_subnet(req))
    {
        wt_log_warn("OTA rejected: request from AP interface");
        return false;
    }
    return true;
}

/*!
    \brief  Handles the firmware branch of POST /api/ota: streams the
            multipart body into the next OTA partition, sets it as the
            boot partition, and reboots on success.
 */
static esp_err_t handler_ota_firmware(httpd_req_t *req)
{
    if (!ota_request_authorized(req))
    {
        RESP_ERR(req, "unauthorized");
        return ESP_OK;
    }

    wt_log_info("OTA firmware: %d bytes incoming", req->content_len);

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
        wt_log_error("esp_ota_end failed: %s", esp_err_to_name(err));
        RESP_ERR(req, "ota write/end failed");
        return ESP_OK;
    }

    if (esp_ota_set_boot_partition(update_part) != ESP_OK)
    {
        RESP_ERR(req, "set boot partition failed");
        return ESP_OK;
    }

    wt_log_info("OTA firmware done rebooting in 2 s");
    RESP_JSON(req, "{\"ok\":true,\"reboot\":true}");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/*!
    \brief  Streams a multipart upload body into a SPIFFS asset file
            (index.html/style.css/app.js), used by the non-firmware
            POST /api/ota targets.
 */
static esp_err_t spiffs_file_upload(httpd_req_t *req, const char *spiffs_path,
                                    const char *label)
{
    if (!ota_request_authorized(req))
    {
        RESP_ERR(req, "unauthorized");
        return ESP_OK;
    }

    wt_log_info("OTA %s: %d bytes incoming", label, req->content_len);

    upload_stream_t stream;
    if (!upload_stream_init(req, &stream))
    {
        RESP_ERR(req, "invalid upload content-type");
        return ESP_OK;
    }

    FILE *f = fopen(spiffs_path, "w");
    if (!f)
    {
        RESP_ERR(req, "cannot open spiffs file");
        return ESP_OK;
    }

    char buf[512];
    int remaining = req->content_len;
    bool ok = true;

    while (remaining > 0)
    {
        int to_recv = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
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
        wt_log_info("OTA %s: done", label);
        RESP_JSON(req, "{\"ok\":true}");
    }
    else
    {
        wt_log_error("OTA %s: write error", label);
        RESP_JSON(req, "{\"ok\":false,\"error\":\"write error\"}");
    }
    return ESP_OK;
}

/*!
    \brief  POST /api/ota handler: dispatches the upload to the firmware
            OTA path or the matching SPIFFS asset target based on the
            ?target= query parameter.
 */
static esp_err_t handler_ota(httpd_req_t *req)
{
    static const struct
    {
        const char *target;
        const char *spiffs_path;
    } s_ota_targets[] = {
        {"index.html", INDEX_HTML},
        {"style.css", STYLE_CSS},
        {"app.js", APP_JS},
    };

    char query[64] = {0};
    char target[32] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "target", target, sizeof(target));

    if (strcmp(target, "firmware") == 0)
    {
        return handler_ota_firmware(req);
    }

    for (size_t i = 0; i < sizeof(s_ota_targets) / sizeof(s_ota_targets[0]); i++)
    {
        if (strcmp(target, s_ota_targets[i].target) == 0)
        {
            return spiffs_file_upload(req, s_ota_targets[i].spiffs_path, s_ota_targets[i].target);
        }
    }

    RESP_ERR(req, "unknown ota target");
    return ESP_OK;
}

/*!
    \brief  Sends a single WS text frame back to the requesting client.
 */
static void ws_reply(httpd_req_t *req, const char *text)
{
    httpd_ws_frame_t pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    esp_err_t err = httpd_ws_send_frame(req, &pkt);
    if (err != ESP_OK)
    {
        wt_log_warn("ws_reply: httpd_ws_send_frame failed (%s) len=%u fd=%d",
                 esp_err_to_name(err), (unsigned)pkt.len, httpd_req_to_sockfd(req));
    }
}

#define WT_WS_PROTO_VERSION 1

/*!
    \brief  Sent once to a client right after its WS handshake completes.
            Lets the SPA detect a firmware/UI protocol mismatch (e.g. after
            a partial OTA of only index.html/app.js) instead of silently
            rendering with missing fields.
 */
static void ws_send_hello(httpd_req_t *req)
{
    char hello[160];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"proto\":%d,"
             "\"features\":[\"display\",\"wifi\",\"power\",\"settings\",\"logs\"]}",
             WT_WS_PROTO_VERSION);
    ws_reply(req, hello);
}

/*!
    \brief  Sent once to a client right after ws_send_hello(), immediately after
            its WS handshake completes. Carries every field that is fixed for the
            lifetime of the current boot (chip identity, build info, OTA slot,
            reset reason) so the recurring "full" broadcast (every 2 s, forever)
            doesn't have to re-serialize and re-send unchanging strings on every
            tick see build_ws_payload() below, which only carries fields that
            can actually change after boot.
 */
static void ws_send_info(httpd_req_t *req)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *app1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    char ota_slot[8] = "app0";
    if (running && app1 && running->address == app1->address)
        strlcpy(ota_slot, "app1", sizeof(ota_slot));

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

    const esp_app_desc_t *app_desc = esp_app_get_description();

    char info[384];
    snprintf(info, sizeof(info),
             "{\"type\":\"info\","
             "\"chip_model\":\"%s\",\"cpu_cores\":%d,\"cpu_freq_mhz\":240,"
             "\"flash_size\":%u,"
             "\"app_version\":\"%s\",\"build_date\":\"%s %s\","
             "\"idf_version\":\"%s\",\"ota_slot\":\"%s\","
             "\"app0_state\":\"valid\",\"app1_state\":\"empty\","
             "\"min_leds\":%d,\"max_leds\":%d,"
             "\"reset_reason\":\"%s\"}",
             chip_name, chip.cores, (unsigned)flash_size,
             app_desc->version, __DATE__, __TIME__, esp_get_idf_version(), ota_slot,
             WT_SEGD_TOTAL_LEDS, WT_SEGD_MAX_TOTAL_LEDS,
             reset_reason);
    ws_reply(req, info);
}

/*!
    \brief  Parses an inbound WS JSON command and dispatches it to the
            matching handler (ping/settings/ntp_sync/reboot/wifi), replying
            with an ack frame.
 */
static void ws_dispatch(httpd_req_t *req, const char *json_str)
{
    cJSON *j = cJSON_Parse(json_str);
    if (!j)
    {
        ws_reply(req, "{\"ack\":\"err\",\"msg\":\"json parse error\"}");
        return;
    }

    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(j, "cmd"));
    if (!cmd)
    {
        cJSON_Delete(j);
        ws_reply(req, "{\"ack\":\"err\",\"msg\":\"missing cmd\"}");
        return;
    }

    if (strcmp(cmd, "ping") == 0)
    {
        cJSON_Delete(j);
        ws_reply(req, "{\"ack\":\"pong\"}");
        return;
    }

    if (strcmp(cmd, "settings") == 0)
    {
        wt_settings_t s = wt_settings_get();
        settings_apply_json(&s, j);
        cJSON_Delete(j);
        bool ok = wt_settings_set(&s);
        /* wt_settings_set() notifies the app_main display-request loop, which rebuilds and pushes
           the display request from the new settings no need to do it here too. */
        ws_reply(req, ok ? "{\"ack\":\"ok\"}" : "{\"ack\":\"err\",\"msg\":\"nvs write failed\"}");
        return;
    }

    if (strcmp(cmd, "ntp_sync") == 0)
    {
        cJSON *srv = cJSON_GetObjectItem(j, "server");
        if (srv && cJSON_IsString(srv))
        {
            wt_settings_t s = wt_settings_get();
            strlcpy(s.ntp_server, cJSON_GetStringValue(srv), sizeof(s.ntp_server));
            wt_settings_set(&s);
        }
        cJSON_Delete(j);
        wt_log_info("NTP sync requested via WS");
        ws_reply(req, "{\"ack\":\"ok\"}");
        return;
    }

    if (strcmp(cmd, "reboot") == 0)
    {
        cJSON_Delete(j);
        ws_reply(req, "{\"ack\":\"ok\",\"reboot\":true}");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    if (strcmp(cmd, "wifi") == 0)
    {
        const char *op = cJSON_GetStringValue(cJSON_GetObjectItem(j, "op"));
        const char *err_msg = "invalid op";
        bool ok = false;

        if (op && strcmp(op, "add") == 0)
        {
            const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
            cJSON *passj = cJSON_GetObjectItem(j, "password");
            const char *passwd = passj ? cJSON_GetStringValue(passj) : "";
            ok = ssid && wt_wifi_add_profile(ssid, passwd ? passwd : "");
            err_msg = "full or invalid ssid";
        }
        else if (op && strcmp(op, "del") == 0)
        {
            cJSON *idxj = cJSON_GetObjectItem(j, "index");
            int idx = idxj ? (int)cJSON_GetNumberValue(idxj) : -1;
            ok = (idx >= 0) && wt_wifi_remove_profile(idx);
            err_msg = "invalid index";
        }
        else if (op && strcmp(op, "connect") == 0)
        {
            cJSON *idxj = cJSON_GetObjectItem(j, "index");
            int idx = idxj ? (int)cJSON_GetNumberValue(idxj) : -1;
            ok = (idx >= 0) && wt_wifi_connect_profile(idx);
            err_msg = "invalid index";
        }

        cJSON_Delete(j);
        if (ok)
        {
            ws_reply(req, "{\"ack\":\"ok\"}");
        }
        else
        {
            char resp[64];
            snprintf(resp, sizeof(resp), "{\"ack\":\"err\",\"msg\":\"%s\"}", err_msg);
            ws_reply(req, resp);
        }
        return;
    }

    cJSON_Delete(j);
    ws_reply(req, "{\"ack\":\"err\",\"msg\":\"unknown cmd\"}");
}

/*!
    \brief  ws_post_handshake_cb for /ws fires exactly once, right after the
            WebSocket upgrade completes (CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT).

            From IDF v6.0.1, the main .handler is no longer invoked for the
            handshake itself (only for actual frames), so connection-time
            setup registering the fd for broadcasts and sending the
            one-time hello/info frames has to live here instead of behind
            a req->method == HTTP_GET check in handler_ws().
 */
static esp_err_t ws_on_connect(httpd_req_t *req)
{
    ws_client_add(httpd_req_to_sockfd(req));
    ws_send_hello(req);
    ws_send_info(req);
    return ESP_OK;
}

/*!
    \brief  /ws frame handler: receives one WS frame, handles close
            frames and connection housekeeping, and forwards text
            payloads to ws_dispatch().
 */
static esp_err_t handler_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK)
    {
        ws_client_remove(fd);
        return ret;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE)
    {
        ws_client_remove(fd);
        return ESP_OK;
    }
    if (pkt.type != HTTPD_WS_TYPE_TEXT || pkt.len == 0)
        return ESP_OK;

    if (pkt.len > WS_MAX_INBOUND_PAYLOAD)
    {
        wt_log_warn("WS rx: frame too large (%u bytes) dropping connection", (unsigned)pkt.len);
        ws_client_remove(fd);
        return ESP_FAIL;
    }

    uint8_t *buf = (uint8_t *)malloc(pkt.len + 1);
    if (!buf)
    {
        wt_log_error("WS rx: OOM");
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK)
    {
        free(buf);
        ws_client_remove(fd);
        return ret;
    }

    buf[pkt.len] = '\0';
    ws_dispatch(req, (const char *)buf);
    free(buf);
    return ESP_OK;
}

/*!
    \brief  Build the periodic "full" WS frame.

    \param[in]  force  When true, rebuild and include the wifi/settings/power
                    sub-objects even if their generation counters haven't
                    changed (used to give a freshly connected client an
                    initial snapshot). Otherwise those sub-objects are only
                    rebuilt when their underlying state actually changed
                    since the last call see wt_settings_get_generation()/
                    wt_wifi_get_generation() and are sent as JSON null
                    otherwise, so an idle device isn't rebuilding/re-sending
                    the same bytes on every tick.
 */
static void build_ws_payload(char *buf, size_t buf_len, uint32_t *inout_log_seq, bool force)
{
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    uint32_t total_heap = min_heap + free_heap;
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    size_t spiffs_total = 0, spiffs_used = 0;
    esp_spiffs_info("spiffs", &spiffs_total, &spiffs_used);
    int spiffs_pct = spiffs_total ? (int)(spiffs_used * 100 / spiffs_total) : 0;

    wt_wifi_status_t wst = wt_wifi_get_status();
    float temperature_c = NAN;
    bool has_temp = read_mcu_temperature_c(&temperature_c);

    /* ---- Display ---- */
    static char disp_json[2048];
    strlcpy(disp_json, "null", sizeof(disp_json));
    wt_segd_snapshot_t snap;
    if (wt_segd_snapshot_get(&snap))
    {
        int brightness = (snap.request.intensity * 100) / 255;
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "available", true);
        cJSON_AddStringToObject(d, "mode", mode_to_string(snap.request.mode));
        cJSON_AddStringToObject(d, "anim", anim_to_string(snap.request.anim));
        cJSON_AddNumberToObject(d, "brightness", brightness);
        cJSON_AddBoolToObject(d, "colon", snap.colon_on);
        cJSON_AddBoolToObject(d, "colon_blink", snap.request.colon_blink);
        cJSON_AddNumberToObject(d, "time_format", snap.request.time_format);
        cJSON *digits = cJSON_AddArrayToObject(d, "digits");
        for (int i = 0; i < WT_SEGD_NUM_DIGITS; i++)
            cJSON_AddItemToArray(digits, cJSON_CreateNumber(snap.frame.digit[i]));
        cJSON_AddItemToObject(d, "color_on", color_to_json(snap.request.color_on));
        cJSON_AddItemToObject(d, "color_off", color_to_json(snap.request.color_off));
        cJSON_AddItemToObject(d, "colon_color", color_to_json(snap.colon_color));
        if (!digit_colors_all_match_color_on(&snap))
        {
            cJSON *dc = cJSON_AddArrayToObject(d, "digit_colors");
            for (int di = 0; di < WT_SEGD_NUM_DIGITS; di++)
            {
                cJSON *sc = cJSON_CreateArray();
                for (int si = 0; si < WT_SEGD_SEGS_PER_DIGIT; si++)
                    cJSON_AddItemToArray(sc, color_to_json(snap.digit_color[di][si]));
                cJSON_AddItemToArray(dc, sc);
            }
        }
        char *ds = cJSON_PrintUnformatted(d);
        if (ds)
        {
            strlcpy(disp_json, ds, sizeof(disp_json));
            free(ds);
        }
        cJSON_Delete(d);
    }

    /* ---- Logs (incremental) ---- */
    static char log_json[2048];
    uint32_t next_seq = 0;
    wt_log_read_json(*inout_log_seq, log_json, sizeof(log_json), &next_seq);
    *inout_log_seq = next_seq;

    /* ---- WiFi (tier 2 only rebuilt when wt_wifi_get_generation() changes) ---- */
    static uint32_t s_last_wifi_gen = 0;
    static char wifi_json[1024];
    uint32_t wifi_gen = wt_wifi_get_generation();
    bool wifi_dirty = force || (wifi_gen != s_last_wifi_gen);
    s_last_wifi_gen = wifi_gen;

    if (wifi_dirty)
    {
        wt_wifi_profile_t prof[WT_WIFI_MAX_PROFILES];
        int cnt = wt_wifi_get_profiles(prof, WT_WIFI_MAX_PROFILES);
        cJSON *w = cJSON_CreateObject();
        cJSON_AddBoolToObject(w, "connected", wst.sta_connected);
        cJSON_AddStringToObject(w, "ssid", wst.sta_ssid);
        cJSON_AddStringToObject(w, "ip", wst.sta_ip);
        cJSON_AddNumberToObject(w, "rssi", wst.sta_rssi);
        cJSON_AddNumberToObject(w, "channel", 0);
        cJSON_AddStringToObject(w, "ap_ip", wst.ap_ip);
        cJSON_AddBoolToObject(w, "ap_active", wst.ap_active);
        cJSON_AddStringToObject(w, "ap_ssid", wst.ap_ssid);
        cJSON_AddNumberToObject(w, "ap_clients", wst.ap_clients);
        cJSON *profiles = cJSON_AddArrayToObject(w, "profiles");
        for (int i = 0; i < cnt; i++)
        {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "idx", i);
            cJSON_AddStringToObject(p, "ssid", prof[i].ssid);
            cJSON_AddBoolToObject(p, "has_pass", strlen(prof[i].passwd) > 0);
            cJSON_AddItemToArray(profiles, p);
        }
        char *ws = cJSON_PrintUnformatted(w);
        if (ws)
        {
            strlcpy(wifi_json, ws, sizeof(wifi_json));
            free(ws);
        }
        else
        {
            strlcpy(wifi_json, "null", sizeof(wifi_json));
        }
        cJSON_Delete(w);
    }
    else
    {
        strlcpy(wifi_json, "null", sizeof(wifi_json));
    }

    /* ---- Power + Settings (tier 2 only rebuilt when wt_settings_get_generation()
       changes; both are derived entirely from wt_settings_t) ---- */
    static uint32_t s_last_settings_gen = 0;
    static char power_json[256];
    static char settings_json[1280];
    uint32_t settings_gen = wt_settings_get_generation();
    bool settings_dirty = force || (settings_gen != s_last_settings_gen);
    s_last_settings_gen = settings_gen;

    if (settings_dirty)
    {
        wt_settings_t ss = wt_settings_get();

        cJSON *s_obj = settings_to_json_object(&ss);
        char *ss_str = cJSON_PrintUnformatted(s_obj);
        strlcpy(settings_json, ss_str ? ss_str : "null", sizeof(settings_json));
        if (ss_str)
            free(ss_str);
        cJSON_Delete(s_obj);

        static const char *const s_power_settings_keys[] = {
            "sleep_mode", "sleep_timeout", "batt_alert_pct", "ps_dim", "ps_wifi"};
        cJSON *p_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(p_obj, "battery_pct", 0);
        cJSON_AddNumberToObject(p_obj, "voltage", 0.0);
        cJSON_AddNumberToObject(p_obj, "current", 0);
        cJSON_AddStringToObject(p_obj, "source", "USB");
        cJSON_AddNumberToObject(p_obj, "eta_hours", 0);
        for (size_t i = 0; i < sizeof(s_power_settings_keys) / sizeof(s_power_settings_keys[0]); i++)
        {
            const wt_settings_field_t *f = find_settings_field(s_power_settings_keys[i]);
            if (f)
                settings_field_to_json(p_obj, &ss, f);
        }
        char *ps_str = cJSON_PrintUnformatted(p_obj);
        strlcpy(power_json, ps_str ? ps_str : "null", sizeof(power_json));
        if (ps_str)
            free(ps_str);
        cJSON_Delete(p_obj);
    }
    else
    {
        strlcpy(power_json, "null", sizeof(power_json));
        strlcpy(settings_json, "null", sizeof(settings_json));
    }

    snprintf(buf, buf_len,
             "{\"type\":\"full\","
             "\"cpu_usage\":0,\"flash_used_pct\":0,"
             "\"free_heap\":%lu,\"total_heap\":%lu,\"min_free_heap\":%lu,"
             "\"spiffs_used_pct\":%d,\"uptime_s\":%lld,"
             "\"temperature\":%.1f,"
             "\"sta_connected\":%s,\"sta_ssid\":\"%s\","
             "\"sta_ip\":\"%s\",\"rssi\":%d,\"ap_ip\":\"%s\","
             "\"display\":%s,\"wifi\":%s,\"power\":%s,\"settings\":%s,\"logs\":%s}",
             free_heap, total_heap, min_heap,
             spiffs_pct, (long long)uptime_s,
             has_temp ? temperature_c : 0.0f,
             wst.sta_connected ? "true" : "false", wst.sta_ssid,
             wst.sta_ip, wst.sta_rssi, wst.ap_ip,
             disp_json, wifi_json, power_json, settings_json, log_json);
}

/*!
    \brief  Appends a printf-formatted fragment to a JSON buffer at a
            running offset, tracking overflow.
 */
static bool append_jsonf(char *buf, size_t buf_len, size_t *offset, const char *fmt, ...)
{
    if (!buf || !offset || *offset >= buf_len)
    {
        return false;
    }

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + *offset, buf_len - *offset, fmt, args);
    va_end(args);

    if (written < 0 || (size_t)written >= (buf_len - *offset))
    {
        if (buf_len > 0)
        {
            buf[buf_len - 1] = '\0';
        }
        return false;
    }

    *offset += (size_t)written;
    return true;
}

/*!
    \brief  Serializes a display snapshot into the "disp"/"display" JSON
            fragment shared by the WS display-only and full frames.
 */
static void format_display_json(char *buf, size_t buf_len, const wt_segd_snapshot_t *snap, bool with_type)
{
    if (!buf || buf_len == 0)
    {
        return;
    }

    if (!snap)
    {
        snprintf(buf, buf_len,
                 with_type ? "{\"type\":\"disp\",\"display\":null}" : "null");
        return;
    }

    int brightness = (snap->request.intensity * 100) / 255;
    size_t offset = 0;

    if (!append_jsonf(buf, buf_len, &offset,
                      with_type ? "{\"type\":\"disp\",\"display\":{" : "{"))
    {
        goto overflow;
    }

    if (!append_jsonf(buf, buf_len, &offset,
                      "\"available\":true,"
                      "\"mode\":\"%s\","
                      "\"anim\":\"%s\","
                      "\"brightness\":%d,"
                      "\"colon\":%s,"
                      "\"colon_blink\":%s,"
                      "\"time_format\":%u,"
                      "\"digits\":[%u,%u,%u,%u],"
                      "\"color_on\":[%u,%u,%u],"
                      "\"color_off\":[%u,%u,%u],"
                      "\"colon_color\":[%u,%u,%u]",
                      mode_to_string(snap->request.mode),
                      anim_to_string(snap->request.anim),
                      brightness,
                      snap->colon_on ? "true" : "false",
                      snap->request.colon_blink ? "true" : "false",
                      snap->request.time_format,
                      snap->frame.digit[0], snap->frame.digit[1],
                      snap->frame.digit[2], snap->frame.digit[3],
                      snap->request.color_on.red, snap->request.color_on.green, snap->request.color_on.blue,
                      snap->request.color_off.red, snap->request.color_off.green, snap->request.color_off.blue,
                      snap->colon_color.red, snap->colon_color.green, snap->colon_color.blue))
    {
        goto overflow;
    }

    /* Per-segment colors are only sent when they diverge from color_on  
       the common "single color for the whole display" case just omits the
       key, and the client already falls back to color_on/brightness. This
       is the highest-frequency frame in the protocol (~20 fps), so skipping
       the ~84-number array here matters far more than in the "full" frame. */
    if (!digit_colors_all_match_color_on(snap))
    {
        if (!append_jsonf(buf, buf_len, &offset, ",\"digit_colors\":["))
        {
            goto overflow;
        }

        for (int di = 0; di < WT_SEGD_NUM_DIGITS; ++di)
        {
            if (!append_jsonf(buf, buf_len, &offset, di == 0 ? "[" : ",["))
            {
                goto overflow;
            }

            for (int si = 0; si < WT_SEGD_SEGS_PER_DIGIT; ++si)
            {
                wt_segd_color_t color = snap->digit_color[di][si];
                if (!append_jsonf(buf, buf_len, &offset,
                                  si == 0 ? "[%u,%u,%u]" : ",[%u,%u,%u]",
                                  color.red, color.green, color.blue))
                {
                    goto overflow;
                }
            }

            if (!append_jsonf(buf, buf_len, &offset, "]"))
            {
                goto overflow;
            }
        }

        if (!append_jsonf(buf, buf_len, &offset, "]"))
        {
            goto overflow;
        }
    }

    if (!append_jsonf(buf, buf_len, &offset, with_type ? "}}" : "}"))
    {
        goto overflow;
    }
    return;

overflow:
    snprintf(buf, buf_len, with_type ? "{\"type\":\"disp\",\"display\":null}" : "null");
}

/*!
    \brief  Builds the periodic display-only WS frame ("disp" type) from
            the current segment-display snapshot.
 */
static void build_display_frame(char *buf, size_t buf_len)
{
    wt_segd_snapshot_t snap;
    if (!wt_segd_snapshot_get(&snap))
    {
        snprintf(buf, buf_len, "{\"type\":\"disp\",\"display\":null}");
        return;
    }
    format_display_json(buf, buf_len, &snap, true);
}

/*!
    \brief  Broadcasts a WS text payload to every connected client,
            closing any connection whose send fails.
 */
static void ws_broadcast_frame(const char *payload)
{
    httpd_ws_frame_t pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)payload,
        .len = strlen(payload),
    };
    if (!s_ws_mutex || xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (s_ws_fds[i] < 0)
            continue;
        esp_err_t err = httpd_ws_send_frame_async(s_http_server, s_ws_fds[i], &pkt);
        if (err != ESP_OK)
        {
            /* A failed async send can leave the TCP socket half-open without ever
               notifying the browser, so its WebSocket.onclose never fires and the
               client-side auto-reconnect never triggers. Explicitly ask the server
               to tear the session down so the client actually sees a close and
               reconnects instead of sitting on stale data indefinitely. */
            wt_log_warn("WS send failed fd=%d (%s) closing", s_ws_fds[i], esp_err_to_name(err));
            httpd_sess_trigger_close(s_http_server, s_ws_fds[i]);
            s_ws_fds[i] = -1;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

#define CAPTIVE_AP_IP "192.168.4.1"
#define CAPTIVE_DNS_PORT 53
#define CAPTIVE_DNS_BUF_SIZE 512

/*!
    \brief  Answers a captive-portal DNS query in place, rewriting it into
            an authoritative response that points every A/ANY query at the
            AP's own IP address. Called from wt_task_web after recvfrom()
            returns >=12 bytes.
 */
static void dns_handle_query(uint8_t *buf, int len, int sock,
                             struct sockaddr_in *client, socklen_t clen)
{
    if (len < 12)
        return;

    /* Only handle standard queries (QR=0, Opcode=0) */
    if ((buf[2] & 0xFA) != 0x00)
        return;

    /* Patch DNS header in-place */
    uint8_t rd = buf[2] & 0x01; /* preserve Recursion Desired bit */
    buf[2] = 0x84 | rd;         /* QR=1 (response), AA=1 (authoritative) */
    buf[3] = 0x80;              /* RA=1, RCODE=0 (no error)              */
    buf[6] = buf[4];            /* ANCOUNT = QDCOUNT                     */
    buf[7] = buf[5];
    buf[8] = buf[9] = buf[10] = buf[11] = 0; /* NSCOUNT / ARCOUNT = 0      */

    /* Walk the question section to find its end */
    int pos = 12;
    while (pos < len && buf[pos] != 0x00)
    {
        if ((buf[pos] & 0xC0) == 0xC0)
        {
            pos += 2;
            goto past_name;
        }
        pos += buf[pos] + 1;
    }
    if (pos < len && buf[pos] == 0x00)
        pos++; /* consume null label */
past_name:

    if (pos + 4 > len)
    {
        sendto(sock, buf, len, 0, (struct sockaddr *)client, clen);
        return;
    }

    uint16_t qtype = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
    pos += 4; /* skip QTYPE + QCLASS */

    /* Append A-record answer for type A (1) or ANY (255) */
    if ((qtype == 1 || qtype == 255) && pos + 16 <= CAPTIVE_DNS_BUF_SIZE)
    {
        buf[pos++] = 0xC0;
        buf[pos++] = 0x0C; /* name ptr → question  */
        buf[pos++] = 0x00;
        buf[pos++] = 0x01; /* type A               */
        buf[pos++] = 0x00;
        buf[pos++] = 0x01; /* class IN             */
        buf[pos++] = 0x00;
        buf[pos++] = 0x00;
        buf[pos++] = 0x00;
        buf[pos++] = 0x0A; /* TTL 10 s             */
        buf[pos++] = 0x00;
        buf[pos++] = 0x04; /* RDLENGTH 4           */
        buf[pos++] = 192;
        buf[pos++] = 168;
        buf[pos++] = 4;
        buf[pos++] = 1; /* 192.168.4.1          */
    }

    sendto(sock, buf, pos, 0, (struct sockaddr *)client, clen);
}

/*!
    \brief  Answers the iOS/macOS captive-portal probe (GET
            /hotspot-detect.html, /library/test/success.html) with a 302
            redirect must not return 200+"Success" (that would mean
            "has internet") so the OS shows the Captive Network Assistant.
 */
static esp_err_t handler_captive_apple(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" CAPTIVE_AP_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
                       "<html><body>"
                       "<a href=\"http://" CAPTIVE_AP_IP "/\">Sign in to network</a>"
                       "</body></html>");
    return ESP_OK;
}

/*!
    \brief  Answers the Android/Chrome captive-portal probe (GET
            /generate_204, /gen_204) with a non-204 redirect so the OS
            raises its portal notification.
 */
static esp_err_t handler_captive_android(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" CAPTIVE_AP_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/*!
    \brief  Answers the Windows NCSI captive-portal probe (GET /ncsi.txt)
            with the exact expected "Microsoft NCSI" body.
 */
static esp_err_t handler_captive_win_ncsi(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, "Microsoft NCSI");
    return ESP_OK;
}

/*!
    \brief  Answers the Windows connectivity-test probe (GET
            /connecttest.txt).
 */
static esp_err_t handler_captive_win_connect(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, "Microsoft Connect Test");
    return ESP_OK;
}

/*!
    \brief  Answers the Firefox captive-portal probe (GET /canonical.html)
            with an HTML meta-refresh redirect.
 */
static esp_err_t handler_captive_firefox(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req,
                       "<html><head>"
                       "<meta http-equiv=\"refresh\" content=\"0;url=http://" CAPTIVE_AP_IP "/\">"
                       "</head><body>Redirecting...</body></html>");
    return ESP_OK;
}

/*!
    \brief  Generic captive-portal redirect handler for GET /redirect and
            /success.txt.
 */
static esp_err_t handler_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" CAPTIVE_AP_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
                       "<html><body>"
                       "<a href=\"http://" CAPTIVE_AP_IP "/\">Open portal</a>"
                       "</body></html>");
    return ESP_OK;
}

/*!
    \brief  Starts the HTTP server and registers all static-asset, OTA,
            captive-portal probe, and WebSocket URI handlers.
 */
static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 14; /* 3 assets + 1 OTA + 9 captive probes + 1 WS */
    cfg.stack_size = 16384;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        wt_log_error("Failed to start HTTP server");
        return;
    }

    s_http_server = server;

#define REG(m, u, h)                                                        \
    do                                                                      \
    {                                                                       \
        httpd_uri_t _u = {.uri = (u), .method = (m), .handler = (h)};       \
        esp_err_t _e = httpd_register_uri_handler(server, &_u);             \
        if (_e != ESP_OK)                                                  \
            wt_log_error("Failed to register URI %s: %s", (u), esp_err_to_name(_e)); \
    } while (0)

    /* ── Static assets needed for initial page load only ── */
    REG(HTTP_GET, "/", handler_root);
    REG(HTTP_GET, "/style.css", handler_style_css);
    REG(HTTP_GET, "/app.js", handler_app_js);

    /* ── OTA uploads binary multipart, cannot go over WebSocket ── */
    REG(HTTP_POST, "/api/ota", handler_ota);

    /* ── Captive-portal OS probe handlers ── */
    REG(HTTP_GET, "/hotspot-detect.html", handler_captive_apple);
    REG(HTTP_GET, "/library/test/success.html", handler_captive_apple);
    REG(HTTP_GET, "/generate_204", handler_captive_android);
    REG(HTTP_GET, "/gen_204", handler_captive_android);
    REG(HTTP_GET, "/ncsi.txt", handler_captive_win_ncsi);
    REG(HTTP_GET, "/connecttest.txt", handler_captive_win_connect);
    REG(HTTP_GET, "/redirect", handler_captive_redirect);
    REG(HTTP_GET, "/canonical.html", handler_captive_firefox);
    REG(HTTP_GET, "/success.txt", handler_captive_redirect);

#undef REG

    /* ── WebSocket ALL live data and commands go through here ── */
    {
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = handler_ws,
            .is_websocket = true,
            .handle_ws_control_frames = false,
            .ws_post_handshake_cb = ws_on_connect,
        };
        esp_err_t ws_err = httpd_register_uri_handler(server, &ws_uri);
        if (ws_err != ESP_OK)
            wt_log_error("Failed to register /ws handler: %s", esp_err_to_name(ws_err));
    }

    wt_log_info("HTTP server started: 3 assets, 1 OTA, 9 captive, 1 WS");
}

/*!
    \brief  FreeRTOS task. Pin to Core 0 alongside the WiFi task.
 */
void wt_task_web(void *pvParameters)
{
    // wt_log_info("---------- WEB TASK STARTED ----------");

    /* Initialise WebSocket client table and mutex */
    ws_clients_init();
    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex)
        wt_log_error("Failed to create WS mutex");

    /* Wait for WiFi AP to come up */
    vTaskDelay(pdMS_TO_TICKS(3000));

    mount_spiffs();
    start_server();

    /* ── Captive-portal DNS socket ──
       SO_RCVTIMEO = WS_DISPLAY_INTERVAL_MS paces the main loop:
         - a DNS packet arrives → recvfrom returns early → handle query
         - no packet in 50 ms  → recvfrom returns EAGAIN → do WS work  */
    int dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock >= 0)
    {
        int reuse = 1;
        setsockopt(dns_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct timeval tv_dns = {
            .tv_sec = 0,
            .tv_usec = WS_DISPLAY_INTERVAL_MS * 1000,
        };
        setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_dns, sizeof(tv_dns));

        struct sockaddr_in srv = {
            .sin_family = AF_INET,
            .sin_port = htons(CAPTIVE_DNS_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(dns_sock, (struct sockaddr *)&srv, sizeof(srv)) < 0)
        {
            wt_log_error("Captive DNS: bind() failed");
            close(dns_sock);
            dns_sock = -1;
        }
        else
        {
            wt_log_info("Captive portal DNS running (UDP/53 → %s)", CAPTIVE_AP_IP);
        }
    }
    else
    {
        wt_log_error("Captive DNS: socket() failed");
    }

    /* ── WS broadcast + DNS main loop ── */
    static char disp_payload[WS_DISPLAY_PAYLOAD];
    static char full_payload[WS_PAYLOAD_SIZE];
    static uint8_t dns_buf[CAPTIVE_DNS_BUF_SIZE];
    uint32_t log_seq = 0;
    uint32_t tick_cnt = 0;
    int64_t last_ws_us = esp_timer_get_time();

    for (;;)
    {
        /* 1. DNS receive blocks at most WS_DISPLAY_INTERVAL_MS */
        if (dns_sock >= 0)
        {
            struct sockaddr_in client;
            socklen_t clen = sizeof(client);
            int len = recvfrom(dns_sock, dns_buf, sizeof(dns_buf) - 1, 0,
                               (struct sockaddr *)&client, &clen);
            if (len >= 12)
                dns_handle_query(dns_buf, len, dns_sock, &client, clen);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(WS_DISPLAY_INTERVAL_MS));
        }

        /* 2. WS broadcast fire once per WS_DISPLAY_INTERVAL_MS */
        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_ws_us) < (int64_t)(WS_DISPLAY_INTERVAL_MS * 1000LL))
            continue;

        last_ws_us = now_us;

        /* Any WS clients connected? */
        bool any = false;
        if (s_ws_mutex && xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(10)) == pdTRUE)
        {
            for (int i = 0; i < WS_MAX_CLIENTS; i++)
                if (s_ws_fds[i] >= 0)
                {
                    any = true;
                    break;
                }
            xSemaphoreGive(s_ws_mutex);
        }
        if (!any)
        {
            tick_cnt = 0; /* reset so next connect gets a full frame immediately */
            continue;
        }

        tick_cnt++;
        if (tick_cnt >= WS_FULL_TICKS)
        {
            /* Full-state frame: system + logs every tick; wifi/power/settings
               only when their generation counters changed (or a client just
               connected and needs an initial snapshot) see build_ws_payload(). */
            tick_cnt = 0;
            bool force_tier2 = s_ws_new_client;
            s_ws_new_client = false;
            build_ws_payload(full_payload, sizeof(full_payload), &log_seq, force_tier2);
            ws_broadcast_frame(full_payload);
        }
        else
        {
            /* Display-only frame: cheap snapshot, no system/wifi/settings work */
            build_display_frame(disp_payload, sizeof(disp_payload));
            ws_broadcast_frame(disp_payload);
        }
    }
}
