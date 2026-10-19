//=============================================================================
/*!
    \file    wt_app_web.c
    \brief   Web server: web console, uploads, WebSocket and captive portal.
*/
//=============================================================================

// The endpoints and WebSocket messages are listed in wt_app_web.h.

#include "wt_app_web.h"
#include "wt_app_log.h"
#include "wt_app_wifi.h"
#include "wt_app_settings.h"
#include "wt_seg_display.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "driver/temperature_sensor.h"
#include "cJSON.h"

// Sockets for the captive-portal DNS responder
#include "lwip/sockets.h"
#include "lwip/netdb.h"

// No CORS header is sent: only the page served by this device calls these.
#define RESP_JSON(req, json_str)                                                                   \
    do                                                                                             \
    {                                                                                              \
        httpd_resp_set_type((req), "application/json");                                            \
        httpd_resp_sendstr((req), (json_str));                                                     \
    } while (0)

#define UPLOAD_RECV_RETRIES 6 ///< Receive timeouts in a row (10 s each) before an upload is dropped

typedef struct
{
    bool multipart;
    bool headers_skipped;
    bool done;
    char boundary[80];
    size_t boundary_len;
    char pending[1536];
    size_t pending_len;
    esp_ota_handle_t ota; ///< Where the file data is written
} upload_stream_t;

static temperature_sensor_handle_t s_temp_sensor = NULL;
static bool s_temp_sensor_ready                  = false;

#define WS_MAX_CLIENTS          8     ///< Most WebSocket clients
#define WS_MAX_INBOUND_PAYLOAD  8192  ///< Largest frame accepted from a client
#define WS_DISPLAY_INTERVAL_MS  50    ///< How often the display state is checked
#define WS_DISPLAY_KEEPALIVE_MS 500   ///< Most time between display frames
#define WS_FULL_INTERVAL_MS     2000  ///< Interval between full frames
#define WS_SCAN_PAYLOAD         2048  ///< Buffer for a scan frame
#define WS_DISPLAY_PAYLOAD      1536  ///< Buffer for a display frame
#define WS_PAYLOAD_SIZE         12288 ///< Buffer for a full frame
#define WS_MAX_QUEUED           4     ///< Most broadcasts waiting for the server task
#define WS_FULL_TICKS           (WS_FULL_INTERVAL_MS / WS_DISPLAY_INTERVAL_MS)

static void format_display_json(char *buf, size_t buf_len, const wt_segd_snapshot_t *snap,
                                bool with_type);

static httpd_handle_t s_http_server = NULL;
static int s_ws_fds[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;

// Set when a client connects, so the next full frame carries settings,
// WiFi and power in full instead of "unchanged".
static volatile bool s_ws_new_client = false;

/*!
    \brief  Remove a WebSocket client that has disconnected.
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
    \brief  Answer an HTTP request with 400 and {"error":"<msg>"}.
*/
static void resp_error(httpd_req_t *req, const char *msg)
{
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, "400 Bad Request");
    RESP_JSON(req, body);
}

/*!
    \brief  Find a byte sequence in a buffer that need not be NUL-terminated.
*/
static const char *find_bytes(const char *buf, size_t buf_len, const char *needle,
                              size_t needle_len)
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
    \brief  Read the upload's Content-Type: detect a multipart upload and its
            boundary.
*/
static bool upload_stream_init(httpd_req_t *req, upload_stream_t *st)
{
    memset(st, 0, sizeof(*st));

    char content_type[128];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) !=
        ESP_OK)
    {
        return true;
    }

    const char *needle = "boundary=";
    char *boundary     = strstr(content_type, needle);
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
    st->boundary_len          = raw_len + 2;
    st->multipart             = true;
    return true;
}

/*!
    \brief  Pass the received file data on, keeping back the last bytes in case
            they are the start of the closing boundary.
*/
static esp_err_t upload_stream_flush_payload(upload_stream_t *st, bool final_flush)
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
        const char *tail = find_bytes(st->pending, st->pending_len, tail_marker, tail_len);
        if (!tail)
        {
            wt_log_error("Upload tail boundary not found");
            return ESP_FAIL;
        }

        size_t payload_len = (size_t)(tail - st->pending);
        if (payload_len > 0 && esp_ota_write(st->ota, st->pending, payload_len) != ESP_OK)
            return ESP_FAIL;

        st->pending_len = 0;
        st->done        = true;
        return ESP_OK;
    }

    if (st->pending_len <= keep_len)
        return ESP_OK;

    size_t flush_len = st->pending_len - keep_len;
    if (esp_ota_write(st->ota, st->pending, flush_len) != ESP_OK)
        return ESP_FAIL;

    memmove(st->pending, st->pending + flush_len, keep_len);
    st->pending_len = keep_len;
    return ESP_OK;
}

/*!
    \brief  Process one received chunk of an upload.
*/
static esp_err_t upload_stream_consume(upload_stream_t *st, const char *chunk, size_t chunk_len)
{
    if (!st->multipart)
        return esp_ota_write(st->ota, chunk, chunk_len);

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
        const char *hdr_end     = find_bytes(st->pending, st->pending_len, sep, sizeof(sep) - 1);
        if (!hdr_end)
            return ESP_OK;

        size_t offset = (size_t)(hdr_end - st->pending) + (sizeof(sep) - 1);
        memmove(st->pending, st->pending + offset, st->pending_len - offset);
        st->pending_len -= offset;
        st->headers_skipped = true;
    }

    return upload_stream_flush_payload(st, false);
}

/*!
    \brief  Read the chip temperature in °C, starting the sensor on first use.
*/
static bool read_mcu_temperature_c(float *out_celsius)
{
    static bool s_temp_sensor_failed = false;

    // The sensor is started on first use; after a failure it is not retried.
    if (!s_temp_sensor_ready && !s_temp_sensor_failed)
    {
        s_temp_sensor_failed            = true;
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t err                   = temperature_sensor_install(&cfg, &s_temp_sensor);
        if (err == ESP_OK)
        {
            err = temperature_sensor_enable(s_temp_sensor);
            if (err != ESP_OK)
                temperature_sensor_uninstall(s_temp_sensor);
        }
        if (err != ESP_OK)
        {
            wt_log_error("Temperature sensor start failed: %s", esp_err_to_name(err));
            s_temp_sensor = NULL;
        }
        else
        {
            s_temp_sensor_failed = false;
            s_temp_sensor_ready  = true;
        }
    }

    return s_temp_sensor_ready &&
           (temperature_sensor_get_celsius(s_temp_sensor, out_celsius) == ESP_OK);
}

/*!
    \brief  Copy a string for use inside a JSON string: quotes and backslashes
            escaped, control characters replaced by spaces.
*/
static void json_escape_string(char *dst, size_t dst_len, const char *src)
{
    size_t out = 0;
    if (!dst || dst_len == 0)
    {
        return;
    }
    while (src && *src && (out + 2) < dst_len)
    {
        unsigned char c = (unsigned char)*src++;
        if (c == '"' || c == '\\')
        {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        }
        else
        {
            dst[out++] = (c < 0x20) ? ' ' : (char)c;
        }
    }
    dst[out] = '\0';
}

/*!
    \brief  Name of a display mode as used in the messages.
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
    \brief  Upper-case text and keep only characters the display can show
            (A-Z, 0-9, space, '_' and '-').
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
        if (((ch >= 'A') && (ch <= 'Z')) || ((ch >= '0') && (ch <= '9')) || (ch == ' ') ||
            (ch == '_') || (ch == '-'))
        {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
}

// Settings are not applied to the display here: wt_settings_set() wakes
// the display-request loop, which builds the display request itself.

typedef enum
{
    WT_SF_STR,           ///< Text
    WT_SF_STR_SANITIZED, ///< Text limited to characters the display can show
    WT_SF_BOOL,
    WT_SF_U8,
    WT_SF_U16,
    WT_SF_I16,
    WT_SF_U8_PCT255, ///< 0-255 value sent as a percentage
    WT_SF_COLOR_HEX, ///< "#rrggbb" color, also sets color_on
} wt_settings_field_type_t;

typedef struct
{
    const char *key;
    wt_settings_field_type_t type;
    size_t offset; ///< Position of the member in wt_settings_t
    size_t size;   ///< Size of the member
} wt_settings_field_t;

// Describes one wt_settings_t member: wire key, type, offset and size.
// clang-format off
#define WT_SF(key_, type_, member_) \
    {(key_), (type_), offsetof(wt_settings_t, member_), sizeof(((wt_settings_t *)0)->member_)}
// clang-format on

static const wt_settings_field_t s_settings_fields[] = {
    WT_SF("color", WT_SF_COLOR_HEX, color_hex),
    WT_SF("brightness", WT_SF_U8_PCT255, intensity),
    WT_SF("anim_colon", WT_SF_BOOL, colon_blink),
    WT_SF("anim_scroll", WT_SF_BOOL, anim_scroll),
    WT_SF("anim_transition", WT_SF_BOOL, anim_transition),
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
    \brief  Read a JSON number as an integer limited to lo..hi.
*/
static int clamp_number(const cJSON *val, int lo, int hi)
{
    double d = cJSON_GetNumberValue(val);
    if (!(d >= lo)) // also true for NaN
    {
        return lo;
    }
    if (d > hi)
    {
        return hi;
    }
    return (int)d;
}

/*!
    \brief  Store one received value in its setting.
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
        // A value of the wrong type is ignored, and numbers are limited to
        // the field's range.
        case WT_SF_BOOL:
            if (cJSON_IsBool(val))
            {
                *(bool *)field = cJSON_IsTrue(val);
            }
            break;
        case WT_SF_U8:
            if (cJSON_IsNumber(val))
            {
                *(uint8_t *)field = (uint8_t)clamp_number(val, 0, UINT8_MAX);
            }
            break;
        case WT_SF_U16:
            if (cJSON_IsNumber(val))
            {
                *(uint16_t *)field = (uint16_t)clamp_number(val, 0, UINT16_MAX);
            }
            break;
        case WT_SF_I16:
            if (cJSON_IsNumber(val))
            {
                *(int16_t *)field = (int16_t)clamp_number(val, INT16_MIN, INT16_MAX);
            }
            break;
        case WT_SF_U8_PCT255:
            if (cJSON_IsNumber(val))
            {
                // Rounded so a percentage converts to 0-255 and back unchanged.
                *(uint8_t *)field = (uint8_t)((clamp_number(val, 0, 100) * 255 + 50) / 100);
            }
            break;
        case WT_SF_COLOR_HEX:
            if (cJSON_IsString(val))
            {
                const char *hex = cJSON_GetStringValue(val);
                strlcpy((char *)field, hex, f->size);
                wt_settings_parse_hex_color(hex, &s->color_on.red, &s->color_on.green,
                                            &s->color_on.blue);
            }
            break;
    }
}

/*!
    \brief  Add one setting to a message.
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
            cJSON_AddNumberToObject(obj, f->key,
                                    ((int)(*(const uint8_t *)field) * 100 + 127) / 255);
            break;
    }
}

// The web console, gzip-compressed and linked into the firmware by
// components/wt_app_web/CMakeLists.txt.
extern const uint8_t _binary_index_html_gz_start[], _binary_index_html_gz_end[];
extern const uint8_t _binary_style_css_gz_start[], _binary_style_css_gz_end[];
extern const uint8_t _binary_app_js_gz_start[], _binary_app_js_gz_end[];

/// A web console file: its URL, content type and compressed bytes.
typedef struct
{
    const char *uri;
    const char *type;
    const uint8_t *start;
    const uint8_t *end;
} web_file_t;

static const web_file_t s_web_files[] = {
    {"/", "text/html", _binary_index_html_gz_start, _binary_index_html_gz_end},
    {"/style.css", "text/css", _binary_style_css_gz_start, _binary_style_css_gz_end},
    {"/app.js", "application/javascript", _binary_app_js_gz_start, _binary_app_js_gz_end},
};

/*!
    \brief  GET of a web console file (user_ctx is its web_file_t), sent
            gzip-compressed from the firmware image.
*/
static esp_err_t handler_web_file(httpd_req_t *req)
{
    const web_file_t *file = (const web_file_t *)req->user_ctx;

    // The files change only with the firmware, so the ETag is taken from the
    // firmware's hash. no-cache makes the browser check it on every load: an
    // update shows at once, while an unchanged file costs a short 304.
    char hash[9], etag[12], match[16] = "";
    esp_app_get_elf_sha256(hash, sizeof(hash));
    snprintf(etag, sizeof(etag), "\"%s\"", hash);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "ETag", etag);
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", match, sizeof(match)) == ESP_OK &&
        strcmp(match, etag) == 0)
    {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    httpd_resp_set_type(req, file->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)file->start, file->end - file->start);
}

/*!
    \brief  POST /api/ota: firmware update. The image (which also carries the
            web console) is written to the next update partition, made the
            boot partition, and the device restarts. Accepted over both the
            access point and the home network.
*/
static esp_err_t handler_ota(httpd_req_t *req)
{
    wt_log_info("OTA: %d bytes incoming", req->content_len);

    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    upload_stream_t stream;
    if (!update_part)
    {
        resp_error(req, "no OTA partition");
        return ESP_OK;
    }
    if (!upload_stream_init(req, &stream) || req->content_len == 0)
    {
        resp_error(req, "invalid upload");
        return ESP_OK;
    }
    if (esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &stream.ota) != ESP_OK)
    {
        resp_error(req, "ota_begin failed");
        return ESP_OK;
    }

    // Receive the body, passing the file data to the update partition.
    const char *error = NULL;
    char buf[1024];
    size_t remaining = req->content_len;
    int timeouts     = 0;
    while (remaining > 0 && !error)
    {
        int to_recv = (remaining < sizeof(buf)) ? (int)remaining : (int)sizeof(buf);
        int n       = httpd_req_recv(req, buf, to_recv);
        if (n <= 0)
        {
            // Give up on a client that stops sending.
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < UPLOAD_RECV_RETRIES)
                continue;
            error = "receive failed";
            break;
        }
        timeouts = 0;
        if (upload_stream_consume(&stream, buf, (size_t)n) != ESP_OK)
            error = "write error";
        remaining -= (size_t)n;
    }

    // Pass on the data held back while looking for the closing boundary.
    if (!error && stream.multipart &&
        (!stream.headers_skipped || upload_stream_flush_payload(&stream, true) != ESP_OK))
        error = "write error";

    if (error)
    {
        esp_ota_abort(stream.ota);
        wt_log_error("OTA: %s", error);
        resp_error(req, error);
        return ESP_OK;
    }

    esp_err_t err = esp_ota_end(stream.ota);
    if (err != ESP_OK)
    {
        wt_log_error("esp_ota_end failed: %s", esp_err_to_name(err));
        resp_error(req, "not a valid firmware image");
        return ESP_OK;
    }
    if (esp_ota_set_boot_partition(update_part) != ESP_OK)
    {
        resp_error(req, "set boot partition failed");
        return ESP_OK;
    }

    wt_log_info("OTA done, restarting in 2 s");
    RESP_JSON(req, "{\"ok\":true,\"reboot\":true}");
    wt_settings_flush();
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/*!
    \brief  Send a text frame to the client that sent the request.
*/
static void ws_reply(httpd_req_t *req, const char *text)
{
    httpd_ws_frame_t pkt = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)text,
        .len        = strlen(text),
    };
    esp_err_t err = httpd_ws_send_frame(req, &pkt);
    if (err != ESP_OK)
    {
        wt_log_warn("ws_reply: httpd_ws_send_frame failed (%s) len=%u fd=%d", esp_err_to_name(err),
                    (unsigned)pkt.len, httpd_req_to_sockfd(req));
    }
}

#define WT_WS_PROTO_VERSION 1

/*!
    \brief  Send the "info" frame once after a client connects: chip, build
            and reset details that do not change while the device runs.
*/
static void ws_send_info(httpd_req_t *req)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *app1 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
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
    const char *chip_name = (chip.model == CHIP_ESP32)     ? "ESP32"
                            : (chip.model == CHIP_ESP32S2) ? "ESP32-S2"
                            : (chip.model == CHIP_ESP32S3) ? "ESP32-S3"
                            : (chip.model == CHIP_ESP32C3) ? "ESP32-C3"
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
             chip_name, chip.cores, (unsigned)flash_size, app_desc->version, __DATE__, __TIME__,
             esp_get_idf_version(), ota_slot, WT_SEGD_TOTAL_LEDS, WT_SEGD_MAX_TOTAL_LEDS,
             reset_reason);
    ws_reply(req, info);
}

/*!
    \brief  Carry out a command received from a client and send the reply.
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
        // Keys that are not settings are ignored.
        for (size_t i = 0; i < WT_SETTINGS_FIELD_COUNT; i++)
        {
            cJSON *val = cJSON_GetObjectItem(j, s_settings_fields[i].key);
            if (val)
                settings_field_apply(&s, &s_settings_fields[i], val);
        }
        cJSON_Delete(j);
        bool ok = wt_settings_set(&s);
        // wt_settings_set() wakes the display-request loop, which applies them.
        ws_reply(req, ok ? "{\"ack\":\"ok\"}" : "{\"ack\":\"err\",\"msg\":\"settings rejected\"}");
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
        wt_wifi_request_ntp_sync();
        ws_reply(req, "{\"ack\":\"ok\"}");
        return;
    }

    if (strcmp(cmd, "reboot") == 0)
    {
        cJSON_Delete(j);
        ws_reply(req, "{\"ack\":\"ok\",\"reboot\":true}");
        wt_settings_flush(); // save pending changes before restarting
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }

    if (strcmp(cmd, "wifi") == 0)
    {
        const char *op      = cJSON_GetStringValue(cJSON_GetObjectItem(j, "op"));
        const char *err_msg = "invalid op";
        bool ok             = false;

        if (op && strcmp(op, "add") == 0)
        {
            const char *ssid   = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
            cJSON *passj       = cJSON_GetObjectItem(j, "password");
            const char *passwd = passj ? cJSON_GetStringValue(passj) : "";
            ok                 = ssid && wt_wifi_add_profile(ssid, passwd ? passwd : "");
            err_msg            = "full or invalid ssid";
        }
        else if (op && strcmp(op, "del") == 0)
        {
            cJSON *idxj = cJSON_GetObjectItem(j, "index");
            int idx     = idxj ? (int)cJSON_GetNumberValue(idxj) : -1;
            ok          = (idx >= 0) && wt_wifi_remove_profile(idx);
            err_msg     = "invalid index";
        }
        else if (op && strcmp(op, "connect") == 0)
        {
            cJSON *idxj = cJSON_GetObjectItem(j, "index");
            int idx     = idxj ? (int)cJSON_GetNumberValue(idxj) : -1;
            ok          = (idx >= 0) && wt_wifi_connect_profile(idx);
            err_msg     = "invalid index";
        }
        else if (op && strcmp(op, "hostname") == 0)
        {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
            ok               = name && wt_wifi_set_hostname(name);
            err_msg          = "name must be 1-31 characters: a-z, 0-9 and -";
        }
        else if (op && strcmp(op, "scan") == 0)
        {
            // The results are sent later in a "scan" frame.
            ok      = wt_wifi_scan_start();
            err_msg = "scan could not start, try again";
        }
        else if (op && strcmp(op, "ap_password") == 0)
        {
            const char *password = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
            ok                   = password && wt_wifi_set_ap_password(password);
            err_msg              = "password must be 8-63 characters";
        }

        cJSON_Delete(j);
        if (ok)
        {
            ws_reply(req, "{\"ack\":\"ok\"}");
        }
        else
        {
            char resp[96];
            snprintf(resp, sizeof(resp), "{\"ack\":\"err\",\"msg\":\"%s\"}", err_msg);
            ws_reply(req, resp);
        }
        return;
    }

    cJSON_Delete(j);
    ws_reply(req, "{\"ack\":\"err\",\"msg\":\"unknown cmd\"}");
}

/*!
    \brief  Called once when a WebSocket client has connected: registers it
            for broadcasts and sends the hello and info frames.
*/
static esp_err_t ws_on_connect(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    if (!s_ws_mutex || xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return ESP_OK;

    bool already  = false;
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

    if (!already)
    {
        s_ws_new_client = true;
        wt_log_info("WS client connected fd=%d", fd);
    }

    // The console checks the protocol version to detect a firmware mismatch.
    char hello[160];
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"proto\":%d,"
             "\"features\":[\"display\",\"wifi\",\"power\",\"settings\",\"logs\"]}",
             WT_WS_PROTO_VERSION);
    ws_reply(req, hello);
    ws_send_info(req);
    return ESP_OK;
}

/*!
    \brief  Handles one frame received from a WebSocket client.
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
    \brief  Print a JSON object into buf ("null" if it cannot) and free it.
*/
static void json_print_to(cJSON *obj, char *buf, size_t buf_len)
{
    char *text = cJSON_PrintUnformatted(obj);
    strlcpy(buf, text ? text : "null", buf_len);
    free(text);
    cJSON_Delete(obj);
}

/*!
    \brief  Build the full frame.
    \param[in]  force  Include the WiFi, power and settings objects even if they have not changed.
*/
static void build_ws_payload(char *buf, size_t buf_len, uint32_t *inout_log_seq, bool force)
{
    uint32_t free_heap  = esp_get_free_heap_size();
    uint32_t min_heap   = esp_get_minimum_free_heap_size();
    uint32_t total_heap = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    int64_t uptime_s    = esp_timer_get_time() / 1000000;

    wt_wifi_status_t wst = wt_wifi_get_status();
    float temperature_c  = NAN;
    bool has_temp        = read_mcu_temperature_c(&temperature_c);

    // Display
    static char disp_json[WS_DISPLAY_PAYLOAD];
    wt_segd_snapshot_t snap;
    format_display_json(disp_json, sizeof(disp_json), wt_segd_snapshot_get(&snap) ? &snap : NULL,
                        false);

    // New log lines
    static char log_json[4096];
    uint32_t next_seq = 0;
    wt_log_read_json(*inout_log_seq, log_json, sizeof(log_json), &next_seq);
    *inout_log_seq = next_seq;

    // WiFi: rebuilt only when it changed
    static uint32_t s_last_wifi_gen = 0;
    static char wifi_json[1024];
    uint32_t wifi_gen = wt_wifi_get_generation();
    bool wifi_dirty   = force || (wifi_gen != s_last_wifi_gen);
    s_last_wifi_gen   = wifi_gen;

    if (wifi_dirty)
    {
        wt_wifi_profile_t prof[WT_WIFI_MAX_PROFILES];
        int cnt  = wt_wifi_get_profiles(prof, WT_WIFI_MAX_PROFILES);
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
        cJSON_AddStringToObject(w, "hostname", wst.hostname);
        cJSON *profiles = cJSON_AddArrayToObject(w, "profiles");
        for (int i = 0; i < cnt; i++)
        {
            cJSON *p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "idx", i);
            cJSON_AddStringToObject(p, "ssid", prof[i].ssid);
            cJSON_AddBoolToObject(p, "has_pass", strlen(prof[i].passwd) > 0);
            cJSON_AddItemToArray(profiles, p);
        }
        json_print_to(w, wifi_json, sizeof(wifi_json));
    }
    else
    {
        strlcpy(wifi_json, "null", sizeof(wifi_json));
    }

    // Power and settings: rebuilt only when the settings changed
    static uint32_t s_last_settings_gen = 0;
    static char power_json[256];
    static char settings_json[1280];
    uint32_t settings_gen = wt_settings_get_generation();
    bool settings_dirty   = force || (settings_gen != s_last_settings_gen);
    s_last_settings_gen   = settings_gen;

    if (settings_dirty)
    {
        wt_settings_t ss = wt_settings_get();

        cJSON *s_obj = cJSON_CreateObject();
        for (size_t i = 0; i < WT_SETTINGS_FIELD_COUNT; i++)
            settings_field_to_json(s_obj, &ss, &s_settings_fields[i]);
        json_print_to(s_obj, settings_json, sizeof(settings_json));

        static const char *const s_power_settings_keys[] = {"sleep_mode", "sleep_timeout",
                                                            "batt_alert_pct", "ps_dim", "ps_wifi"};
        cJSON *p_obj                                     = cJSON_CreateObject();
        cJSON_AddNumberToObject(p_obj, "battery_pct", 0);
        cJSON_AddNumberToObject(p_obj, "voltage", 0.0);
        cJSON_AddNumberToObject(p_obj, "current", 0);
        cJSON_AddStringToObject(p_obj, "source", "USB");
        cJSON_AddNumberToObject(p_obj, "eta_hours", 0);
        for (size_t i = 0; i < sizeof(s_power_settings_keys) / sizeof(s_power_settings_keys[0]);
             i++)
        {
            for (size_t k = 0; k < WT_SETTINGS_FIELD_COUNT; k++)
            {
                if (strcmp(s_settings_fields[k].key, s_power_settings_keys[i]) == 0)
                    settings_field_to_json(p_obj, &ss, &s_settings_fields[k]);
            }
        }
        json_print_to(p_obj, power_json, sizeof(power_json));
    }
    else
    {
        strlcpy(power_json, "null", sizeof(power_json));
        strlcpy(settings_json, "null", sizeof(settings_json));
    }

    // An SSID may contain quotes and backslashes.
    char sta_ssid_json[(2 * WT_WIFI_SSID_LEN) + 1];
    json_escape_string(sta_ssid_json, sizeof(sta_ssid_json), wst.sta_ssid);

    snprintf(buf, buf_len,
             "{\"type\":\"full\","
             "\"cpu_usage\":0,\"flash_used_pct\":0,"
             "\"free_heap\":%" PRIu32 ",\"total_heap\":%" PRIu32 ",\"min_free_heap\":%" PRIu32 ","
             "\"uptime_s\":%lld,"
             "\"temperature\":%.1f,"
             "\"sta_connected\":%s,\"sta_ssid\":\"%s\","
             "\"sta_ip\":\"%s\",\"rssi\":%d,\"ap_ip\":\"%s\","
             "\"display\":%s,\"wifi\":%s,\"power\":%s,\"settings\":%s,\"logs\":%s}",
             free_heap, total_heap, min_heap, (long long)uptime_s, has_temp ? temperature_c : 0.0f,
             wst.sta_connected ? "true" : "false", sta_ssid_json, wst.sta_ip, wst.sta_rssi,
             wst.ap_ip, disp_json, wifi_json, power_json, settings_json, log_json);
}

/*!
    \brief  Append formatted text to a buffer; false if it does not fit.
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
    \brief  Write the display state as JSON, for the display and full frames.
*/
static void format_display_json(char *buf, size_t buf_len, const wt_segd_snapshot_t *snap,
                                bool with_type)
{
    if (!buf || buf_len == 0)
    {
        return;
    }

    if (!snap)
    {
        snprintf(buf, buf_len, with_type ? "{\"type\":\"disp\",\"display\":null}" : "null");
        return;
    }

    int brightness = (snap->request.intensity * 100) / 255;
    size_t offset  = 0;

    if (!append_jsonf(buf, buf_len, &offset, with_type ? "{\"type\":\"disp\",\"display\":{" : "{"))
    {
        goto overflow;
    }

    if (!append_jsonf(
            buf, buf_len, &offset,
            "\"available\":true,"
            "\"mode\":\"%s\","
            "\"brightness\":%d,"
            "\"colon\":%s,"
            "\"colon_blink\":%s,"
            "\"time_format\":%u,"
            "\"digits\":[%u,%u,%u,%u],"
            "\"color_on\":[%u,%u,%u],"
            "\"color_off\":[%u,%u,%u],"
            "\"colon_color\":[%u,%u,%u]",
            mode_to_string(snap->request.mode), brightness, snap->colon_on ? "true" : "false",
            snap->request.colon_blink ? "true" : "false", snap->request.time_format,
            snap->frame.digit[0], snap->frame.digit[1], snap->frame.digit[2], snap->frame.digit[3],
            snap->request.color_on.red, snap->request.color_on.green, snap->request.color_on.blue,
            snap->request.color_off.red, snap->request.color_off.green,
            snap->request.color_off.blue, snap->colon_color.red, snap->colon_color.green,
            snap->colon_color.blue))
    {
        goto overflow;
    }

    // Per-segment colors are sent only when they differ from color_on. This
    // frame is sent often, so leaving out the array matters.
    bool all_color_on = true;
    for (int di = 0; di < WT_SEGD_NUM_DIGITS && all_color_on; di++)
    {
        for (int si = 0; si < WT_SEGD_SEGS_PER_DIGIT; si++)
        {
            wt_segd_color_t c = snap->digit_color[di][si];
            if (c.red != snap->request.color_on.red || c.green != snap->request.color_on.green ||
                c.blue != snap->request.color_on.blue)
            {
                all_color_on = false;
                break;
            }
        }
    }
    if (!all_color_on)
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
                if (!append_jsonf(buf, buf_len, &offset, si == 0 ? "[%u,%u,%u]" : ",[%u,%u,%u]",
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
    \brief  Build the scan frame: whether a scan is running and the networks
            found.
 */
static void build_scan_frame(char *buf, size_t buf_len)
{
    wt_wifi_scan_result_t results[WT_WIFI_SCAN_MAX_RESULTS];
    bool scanning = false;
    int count     = wt_wifi_get_scan_results(results, WT_WIFI_SCAN_MAX_RESULTS, &scanning);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "scan");
    cJSON_AddBoolToObject(root, "scanning", scanning);
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < count; i++)
    {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(n, "rssi", results[i].rssi);
        cJSON_AddNumberToObject(n, "channel", results[i].channel);
        cJSON_AddBoolToObject(n, "secure", results[i].secure);
        cJSON_AddItemToArray(networks, n);
    }

    // If the frame does not fit, send an empty one instead of a cut-off one.
    if (!root || !cJSON_PrintPreallocated(root, buf, (int)buf_len, false))
    {
        snprintf(buf, buf_len, "{\"type\":\"scan\",\"scanning\":false,\"networks\":[]}");
    }
    cJSON_Delete(root);
}

/// A frame waiting to be sent to every client by the server task.
typedef struct
{
    size_t len;
    char text[];
} ws_broadcast_t;

static int s_ws_queued = 0; ///< Broadcasts queued and not yet sent (atomic)

/*!
    \brief  Runs on the HTTP server task: send a queued frame to every
            connected client, closing any connection that fails.
*/
static void ws_broadcast_work(void *arg)
{
    ws_broadcast_t *frame = (ws_broadcast_t *)arg;
    httpd_ws_frame_t pkt  = {
         .final      = true,
         .fragmented = false,
         .type       = HTTPD_WS_TYPE_TEXT,
         .payload    = (uint8_t *)frame->text,
         .len        = frame->len,
    };
    if (s_ws_mutex && xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        for (int i = 0; i < WS_MAX_CLIENTS; i++)
        {
            if (s_ws_fds[i] < 0)
                continue;
            // The socket of a client that left may now belong to an ordinary
            // HTTP request, which must not receive a WebSocket frame.
            if (httpd_ws_get_fd_info(s_http_server, s_ws_fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET)
            {
                s_ws_fds[i] = -1;
                continue;
            }
            esp_err_t err = httpd_ws_send_frame_async(s_http_server, s_ws_fds[i], &pkt);
            if (err != ESP_OK)
            {
                // Close the session so the browser notices and reconnects.
                wt_log_warn("WS send failed fd=%d (%s) closing", s_ws_fds[i], esp_err_to_name(err));
                httpd_sess_trigger_close(s_http_server, s_ws_fds[i]);
                s_ws_fds[i] = -1;
            }
        }
        xSemaphoreGive(s_ws_mutex);
    }
    free(frame);
    __atomic_fetch_sub(&s_ws_queued, 1, __ATOMIC_RELAXED);
}

/*!
    \brief  Queue a frame for every connected client. It is sent by the HTTP
            server task, the same task that answers commands, so frames and
            replies are never written to a socket at the same time.
    \param[in]  max_queued  Skip the frame if this many are already waiting,
                            e.g. while an upload keeps the server task busy.
    \return true if the frame was queued.
*/
static bool ws_broadcast_frame(const char *payload, int max_queued)
{
    if (!s_http_server || __atomic_load_n(&s_ws_queued, __ATOMIC_RELAXED) >= max_queued)
        return false;

    size_t len            = strlen(payload);
    ws_broadcast_t *frame = malloc(sizeof(*frame) + len + 1);
    if (!frame)
        return false;
    frame->len = len;
    memcpy(frame->text, payload, len + 1);

    __atomic_fetch_add(&s_ws_queued, 1, __ATOMIC_RELAXED);
    if (httpd_queue_work(s_http_server, ws_broadcast_work, frame) != ESP_OK)
    {
        __atomic_fetch_sub(&s_ws_queued, 1, __ATOMIC_RELAXED);
        free(frame);
        return false;
    }
    return true;
}

#define CAPTIVE_AP_IP        "192.168.4.1"
#define CAPTIVE_DNS_PORT     53
#define CAPTIVE_DNS_BUF_SIZE 512

/*!
    \brief  Answer a DNS query with the access point's address, so connecting
            devices open the captive portal.
*/
static void dns_handle_query(uint8_t *buf, int len, int sock, struct sockaddr_in *client,
                             socklen_t clen)
{
    if (len < 12)
        return;

    // Only standard queries are answered.
    if ((buf[2] & 0xFA) != 0x00)
        return;

    // Turn the query into a response in place.
    uint8_t rd = buf[2] & 0x01; // keep the Recursion Desired bit
    buf[2]     = 0x84 | rd;     // response, authoritative
    buf[3]     = 0x80;          // recursion available, no error
    buf[6]     = 0;             // answer count, set below
    buf[7]     = 0;
    buf[8] = buf[9] = buf[10] = buf[11] = 0; // no authority or additional records

    // Skip the question name.
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
        pos++; // end of name
past_name:

    if (pos + 4 > len)
    {
        sendto(sock, buf, len, 0, (struct sockaddr *)client, clen);
        return;
    }

    uint16_t qtype = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
    pos += 4; // skip question type and class

    // Answer address queries with the access point's own address.
    if ((qtype == 1 || qtype == 255) && pos + 16 <= CAPTIVE_DNS_BUF_SIZE)
    {
        buf[pos++] = 0xC0;
        buf[pos++] = 0x0C; // pointer to the question name
        buf[pos++] = 0x00;
        buf[pos++] = 0x01; // type A
        buf[pos++] = 0x00;
        buf[pos++] = 0x01; // class IN
        buf[pos++] = 0x00;
        buf[pos++] = 0x00;
        buf[pos++] = 0x00;
        buf[pos++] = 0x0A; // TTL 10 s
        buf[pos++] = 0x00;
        buf[pos++] = 0x04; // 4 address bytes
        buf[pos++] = 192;
        buf[pos++] = 168;
        buf[pos++] = 4;
        buf[pos++] = 1; // 192.168.4.1
        buf[7]     = 1; // one answer
    }

    sendto(sock, buf, pos, 0, (struct sockaddr *)client, clen);
}

/*!
    \brief  Captive-portal check from Apple, Android or another OS: redirect
            to the console so the sign-in window opens.
*/
static esp_err_t handler_captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" CAPTIVE_AP_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<html><body>"
                            "<a href=\"http://" CAPTIVE_AP_IP "/\">Sign in to network</a>"
                            "</body></html>");
    return ESP_OK;
}

/*!
    \brief  Windows connectivity check: answer with the text it expects
            (user_ctx).
*/
static esp_err_t handler_captive_text(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, (const char *)req->user_ctx);
    return ESP_OK;
}

/*!
    \brief  Firefox captive-portal check: redirect to the console.
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
    \brief  Start the HTTP server and register every URL handler.
*/
static void start_server(void)
{
    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 14; // 3 assets, 1 OTA, 9 captive-portal probes, 1 WebSocket
    // Enough sockets for several devices at once, each with a WebSocket and
    // a connection or two for page loads. LWIP_MAX_SOCKETS must leave room
    // for the server's own 3 sockets and the DNS responder.
    cfg.max_open_sockets = 12;
    // When all sockets are in use, close the least recently used one instead
    // of refusing new connections.
    cfg.lru_purge_enable  = true;
    cfg.stack_size        = 16384;
    cfg.recv_wait_timeout = 10;
    // A client that vanished without closing is dropped after 3 s instead of
    // holding up the broadcast to everyone else for longer.
    cfg.send_wait_timeout = 3;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        wt_log_error("Failed to start HTTP server");
        return;
    }

    s_http_server = server;

#define REG(m, u, h, ctx)                                                                          \
    do                                                                                             \
    {                                                                                              \
        httpd_uri_t _u = {.uri = (u), .method = (m), .handler = (h), .user_ctx = (void *)(ctx)};   \
        esp_err_t _e   = httpd_register_uri_handler(server, &_u);                                  \
        if (_e != ESP_OK)                                                                          \
            wt_log_error("Failed to register URI %s: %s", (u), esp_err_to_name(_e));               \
    } while (0)

    // Web console files
    for (size_t i = 0; i < sizeof(s_web_files) / sizeof(s_web_files[0]); i++)
        REG(HTTP_GET, s_web_files[i].uri, handler_web_file, &s_web_files[i]);

    // Firmware and web console uploads
    REG(HTTP_POST, "/api/ota", handler_ota, NULL);

    // Captive-portal probes from phones and computers
    REG(HTTP_GET, "/hotspot-detect.html", handler_captive_redirect, NULL);
    REG(HTTP_GET, "/library/test/success.html", handler_captive_redirect, NULL);
    REG(HTTP_GET, "/generate_204", handler_captive_redirect, NULL);
    REG(HTTP_GET, "/gen_204", handler_captive_redirect, NULL);
    REG(HTTP_GET, "/redirect", handler_captive_redirect, NULL);
    REG(HTTP_GET, "/success.txt", handler_captive_redirect, NULL);
    REG(HTTP_GET, "/ncsi.txt", handler_captive_text, "Microsoft NCSI");
    REG(HTTP_GET, "/connecttest.txt", handler_captive_text, "Microsoft Connect Test");
    REG(HTTP_GET, "/canonical.html", handler_captive_firefox, NULL);

#undef REG

    // WebSocket: all live data and commands
    {
        httpd_uri_t ws_uri = {
            .uri                      = "/ws",
            .method                   = HTTP_GET,
            .handler                  = handler_ws,
            .is_websocket             = true,
            .handle_ws_control_frames = false,
            .ws_post_handshake_cb     = ws_on_connect,
        };
        esp_err_t ws_err = httpd_register_uri_handler(server, &ws_uri);
        if (ws_err != ESP_OK)
            wt_log_error("Failed to register /ws handler: %s", esp_err_to_name(ws_err));
    }

    wt_log_info("HTTP server started: 3 assets, 1 OTA, 9 captive, 1 WS");
}

/*!
    \brief  Web task: starts the HTTP server and the captive-portal DNS
            responder, then sends WebSocket frames to connected clients.
    \param[in]  pvParameters  Unused.
*/
void wt_task_web(void *pvParameters)
{

    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        s_ws_fds[i] = -1;
    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex)
        wt_log_error("Failed to create WS mutex");

    // Wait for the access point to come up.
    vTaskDelay(pdMS_TO_TICKS(3000));

    start_server();

    // The server is running, so a newly updated firmware is confirmed as
    // good. If it had crashed before this, the bootloader would roll back.
    if (s_http_server)
    {
        esp_ota_mark_app_valid_cancel_rollback();
    }

    // Captive-portal DNS socket. Its receive timeout also paces the loop
    // below: it returns when a query arrives or after one display interval.
    int dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dns_sock >= 0)
    {
        int reuse = 1;
        setsockopt(dns_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct timeval tv_dns = {
            .tv_sec  = 0,
            .tv_usec = WS_DISPLAY_INTERVAL_MS * 1000,
        };
        setsockopt(dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_dns, sizeof(tv_dns));

        struct sockaddr_in srv = {
            .sin_family      = AF_INET,
            .sin_port        = htons(CAPTIVE_DNS_PORT),
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

    static char disp_payload[WS_DISPLAY_PAYLOAD];
    static char disp_sent[WS_DISPLAY_PAYLOAD]; // last display frame sent
    static char full_payload[WS_PAYLOAD_SIZE];
    int64_t last_disp_us       = 0;
    uint32_t seen_settings_gen = 0;
    uint32_t seen_wifi_gen     = 0;
    uint32_t seen_scan_gen     = 0;
    static char scan_payload[WS_SCAN_PAYLOAD];
    static uint8_t dns_buf[CAPTIVE_DNS_BUF_SIZE];
    uint32_t log_seq   = 0;
    uint32_t tick_cnt  = 0;
    int64_t last_ws_us = esp_timer_get_time();

    for (;;)
    {
        // 1. Answer a DNS query, or wait one display interval.
        if (dns_sock >= 0)
        {
            struct sockaddr_in client;
            socklen_t clen = sizeof(client);
            int len        = recvfrom(dns_sock, dns_buf, sizeof(dns_buf) - 1, 0,
                                      (struct sockaddr *)&client, &clen);
            if (len >= 12)
                dns_handle_query(dns_buf, len, dns_sock, &client, clen);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(WS_DISPLAY_INTERVAL_MS));
        }

        // 2. Send WebSocket frames, at most once per display interval.
        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_ws_us) < (int64_t)(WS_DISPLAY_INTERVAL_MS * 1000LL))
            continue;

        last_ws_us = now_us;

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
            tick_cnt     = 0;
            disp_sent[0] = '\0'; // send a display frame as soon as a client connects
            continue;
        }

        // A full frame is sent every WS_FULL_INTERVAL_MS, and immediately when
        // a client connects or settings or WiFi state change.
        uint32_t settings_gen = wt_settings_get_generation();
        uint32_t wifi_gen     = wt_wifi_get_generation();
        bool state_changed    = (settings_gen != seen_settings_gen) || (wifi_gen != seen_wifi_gen);
        seen_settings_gen     = settings_gen;
        seen_wifi_gen         = wifi_gen;

        // Scan frame: sent when a scan starts and when it finishes, and to a
        // client that connects later.
        uint32_t scan_gen = wt_wifi_get_scan_generation();
        if ((scan_gen != seen_scan_gen) || (s_ws_new_client && scan_gen != 0))
        {
            build_scan_frame(scan_payload, sizeof(scan_payload));
            if (ws_broadcast_frame(scan_payload, WS_MAX_QUEUED))
                seen_scan_gen = scan_gen;
        }

        tick_cnt++;
        if (tick_cnt >= WS_FULL_TICKS || s_ws_new_client || state_changed)
        {
            // Full frame: system data and logs, plus whatever else changed.
            tick_cnt         = 0;
            bool force_tier2 = s_ws_new_client;
            s_ws_new_client  = false;
            build_ws_payload(full_payload, sizeof(full_payload), &log_seq, force_tier2);
            // If it cannot be queued, a new client still needs the complete
            // frame next time.
            if (!ws_broadcast_frame(full_payload, WS_MAX_QUEUED) && force_tier2)
                s_ws_new_client = true;
        }
        else
        {
            // Display frame: sent only when the display changed, plus a
            // keep-alive so the browser knows the connection is alive.
            wt_segd_snapshot_t snap;
            format_display_json(disp_payload, sizeof(disp_payload),
                                wt_segd_snapshot_get(&snap) ? &snap : NULL, true);
            bool changed   = (strcmp(disp_payload, disp_sent) != 0);
            bool keepalive = (now_us - last_disp_us) >= (int64_t)(WS_DISPLAY_KEEPALIVE_MS * 1000LL);
            // Skipped while another frame is waiting: the next one replaces it.
            if ((changed || keepalive) && ws_broadcast_frame(disp_payload, 1))
            {
                strlcpy(disp_sent, disp_payload, sizeof(disp_sent));
                last_disp_us = now_us;
            }
        }
    }
}
