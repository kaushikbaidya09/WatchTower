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
#include <stdarg.h>
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

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
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

static temperature_sensor_handle_t s_temp_sensor = NULL;
static bool s_temp_sensor_ready = false;

/* ------------------------------------------------------------------ */
/*  WebSocket state                                                    */
/* ------------------------------------------------------------------ */

#define WS_MAX_CLIENTS 4          ///< Maximum allowed clients
#define WS_DISPLAY_INTERVAL_MS 50 ///< Web Display fetch refresh rate
#define WS_FULL_INTERVAL_MS 2000  ///< Full system updates rate (system/wifi/power/logs)
#define WS_DISPLAY_PAYLOAD 1536   ///< display-only frame ~700 B
#define WS_PAYLOAD_SIZE 12288     ///< full frame
#define WS_FULL_TICKS (WS_FULL_INTERVAL_MS / WS_DISPLAY_INTERVAL_MS)

static httpd_handle_t s_http_server = NULL;
static int s_ws_fds[WS_MAX_CLIENTS];
static SemaphoreHandle_t s_ws_mutex = NULL;

static void ws_clients_init(void)
{
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        s_ws_fds[i] = -1;
}

static void ws_client_add(int fd)
{
    if (!s_ws_mutex || xSemaphoreTake(s_ws_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
        return;
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
    {
        if (s_ws_fds[i] < 0)
        {
            s_ws_fds[i] = fd;
            break;
        }
    }
    xSemaphoreGive(s_ws_mutex);
    APPLOG_I("WS client connected fd=%d", fd);
}

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
    APPLOG_I("WS client disconnected fd=%d", fd);
}

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

static esp_err_t upload_stream_consume(upload_stream_t *st, const char *chunk, size_t chunk_len, stream_write_cb_t write_cb, void *ctx)
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

static esp_err_t upload_stream_finish(upload_stream_t *st, stream_write_cb_t write_cb, void *ctx)
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

static bool ensure_temp_sensor_ready(void)
{
    if (s_temp_sensor_ready)
        return true;

    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    esp_err_t err = temperature_sensor_install(&cfg, &s_temp_sensor);
    if (err != ESP_OK)
    {
        APPLOG_E("temperature_sensor_install failed: %s", esp_err_to_name(err));
        s_temp_sensor = NULL;
        return false;
    }

    err = temperature_sensor_enable(s_temp_sensor);
    if (err != ESP_OK)
    {
        APPLOG_E("temperature_sensor_enable failed: %s", esp_err_to_name(err));
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = NULL;
        return false;
    }

    s_temp_sensor_ready = true;
    return true;
}

static bool read_mcu_temperature_c(float *out_celsius)
{
    if (!out_celsius)
        return false;

    if (!ensure_temp_sensor_ready())
        return false;

    esp_err_t err = temperature_sensor_get_celsius(s_temp_sensor, out_celsius);
    if (err != ESP_OK)
    {
        APPLOG_E("temperature_sensor_get_celsius failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

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
    case WT_SEGD_ANIM_SOLID:
    default:
        return "solid";
    }
}

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

static cJSON *color_to_json(wt_segd_color_t color)
{
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(color.red));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(color.green));
    cJSON_AddItemToArray(arr, cJSON_CreateNumber(color.blue));
    return arr;
}

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

static wt_segd_anim_t resolve_runtime_anim(const wt_settings_t *s)
{
    if (strcmp(s->reaction_effect, "rainbow") == 0)
    {
        return WT_SEGD_ANIM_RAINBOW;
    }
    if (s->anim_pulse)
    {
        return WT_SEGD_ANIM_PULSE;
    }
    return WT_SEGD_ANIM_SOLID;
}

static void apply_display_settings_now(const wt_settings_t *s)
{
    if (!s || !wt_segd_queue)
    {
        return;
    }

    wt_segd_request_t req = {
        .mode = WT_SEGD_MODE_TIME,
        .value = s->display_value,
        .time_format = s->time_format,
        .colon = true,
        .colon_blink = s->colon_blink,
        .anim = resolve_runtime_anim(s),
        .color_on = s->color_on,
        .color_off = s->color_off,
        .intensity = s->intensity,
    };

    if (strcmp(s->display_mode, "number") == 0)
    {
        req.mode = WT_SEGD_MODE_NUMBER;
        req.colon = false;
        req.colon_blink = false;
    }
    else if (strcmp(s->display_mode, "text") == 0)
    {
        req.mode = WT_SEGD_MODE_TEXT;
        req.colon = false;
        req.colon_blink = false;
        strlcpy(req.text, s->display_text, sizeof(req.text));
    }

    xQueueOverwrite(wt_segd_queue, &req);
}

/* ------------------------------------------------------------------ */
/*  SPIFFS                                                            */
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

/* ------------------------------------------------------------------ */
/*  GET /                                                             */
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
/*  GET /style.css                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_style_css(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    return serve_file(req, STYLE_CSS, "text/css");
}

/* ------------------------------------------------------------------ */
/*  GET /app.js                                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handler_app_js(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    return serve_file(req, APP_JS, "application/javascript");
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
/*  POST /api/ota/firmware  — OTA firmware update                     */
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
/*  Shared SPIFFS file upload helper                                  */
/*  Streams a multipart/form-data body into a fixed SPIFFS path.     */
/* ------------------------------------------------------------------ */

static esp_err_t spiffs_file_upload(httpd_req_t *req, const char *spiffs_path,
                                    const char *label)
{
    APPLOG_I("OTA %s: %d bytes incoming", label, req->content_len);

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
        APPLOG_I("OTA %s: done", label);
        RESP_JSON(req, "{\"ok\":true}");
    }
    else
    {
        APPLOG_E("OTA %s: write error", label);
        RESP_JSON(req, "{\"ok\":false,\"error\":\"write error\"}");
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/ota/index.html                                          */
/*  POST /api/ota/style.css                                           */
/*  POST /api/ota/app.js                                              */
/* ------------------------------------------------------------------ */

static esp_err_t handler_ota_index(httpd_req_t *req)
{
    return spiffs_file_upload(req, INDEX_HTML, "index.html");
}

static esp_err_t handler_ota_style(httpd_req_t *req)
{
    return spiffs_file_upload(req, STYLE_CSS, "style.css");
}

static esp_err_t handler_ota_appjs(httpd_req_t *req)
{
    return spiffs_file_upload(req, APP_JS, "app.js");
}

/* ------------------------------------------------------------------ */
/*  WebSocket handler  GET /ws                                        */
/* ------------------------------------------------------------------ */

static void ws_reply(httpd_req_t *req, const char *text)
{
    httpd_ws_frame_t pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };
    httpd_ws_send_frame(req, &pkt);
}

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

    if (strcmp(cmd, "settings") == 0 || strcmp(cmd, "display") == 0)
    {
        wt_settings_t s = wt_settings_get();
        cJSON *color = cJSON_GetObjectItem(j, "color");
        if (color && cJSON_IsString(color))
        {
            const char *hex = cJSON_GetStringValue(color);
            strlcpy(s.color_hex, hex, sizeof(s.color_hex));
            wt_settings_parse_hex_color(hex, &s.color_on.red, &s.color_on.green, &s.color_on.blue);
        }
        cJSON *brt = cJSON_GetObjectItem(j, "brightness");
        if (brt)
            s.intensity = (uint8_t)(((int)cJSON_GetNumberValue(brt) * 255) / 100);
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
        cJSON *dmod = cJSON_GetObjectItem(j, "display_mode");
        if (dmod && cJSON_IsString(dmod))
            strlcpy(s.display_mode, cJSON_GetStringValue(dmod), sizeof(s.display_mode));
        cJSON *dval = cJSON_GetObjectItem(j, "display_value");
        if (dval)
            s.display_value = (int16_t)cJSON_GetNumberValue(dval);
        cJSON *dtxt = cJSON_GetObjectItem(j, "display_text");
        if (dtxt && cJSON_IsString(dtxt))
            sanitize_display_text(cJSON_GetStringValue(dtxt), s.display_text, sizeof(s.display_text));
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
        if (ok)
        {
            wt_settings_t a = wt_settings_get();
            apply_display_settings_now(&a);
        }
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
        APPLOG_I("NTP sync requested via WS");
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

    if (strcmp(cmd, "wifi_add") == 0)
    {
        const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid"));
        cJSON *passj = cJSON_GetObjectItem(j, "password");
        const char *passwd = passj ? cJSON_GetStringValue(passj) : "";
        bool ok = ssid && wt_wifi_add_profile(ssid, passwd ? passwd : "");
        cJSON_Delete(j);
        ws_reply(req, ok ? "{\"ack\":\"ok\"}" : "{\"ack\":\"err\",\"msg\":\"full or invalid ssid\"}");
        return;
    }

    if (strcmp(cmd, "wifi_del") == 0)
    {
        cJSON *idxj = cJSON_GetObjectItem(j, "index");
        int idx = idxj ? (int)cJSON_GetNumberValue(idxj) : -1;
        cJSON_Delete(j);
        bool ok = (idx >= 0) && wt_wifi_remove_profile(idx);
        ws_reply(req, ok ? "{\"ack\":\"ok\"}" : "{\"ack\":\"err\",\"msg\":\"invalid index\"}");
        return;
    }

    if (strcmp(cmd, "wifi_connect") == 0)
    {
        cJSON *idxj = cJSON_GetObjectItem(j, "index");
        int idx = idxj ? (int)cJSON_GetNumberValue(idxj) : -1;
        cJSON_Delete(j);
        bool ok = (idx >= 0) && wt_wifi_connect_profile(idx);
        ws_reply(req, ok ? "{\"ack\":\"ok\"}" : "{\"ack\":\"err\",\"msg\":\"invalid index\"}");
        return;
    }

    cJSON_Delete(j);
    ws_reply(req, "{\"ack\":\"err\",\"msg\":\"unknown cmd\"}");
}

static esp_err_t handler_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET)
    {
        ws_client_add(httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK)
    {
        ws_client_remove(httpd_req_to_sockfd(req));
        return ret;
    }

    if (pkt.type == HTTPD_WS_TYPE_CLOSE)
    {
        ws_client_remove(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (pkt.type != HTTPD_WS_TYPE_TEXT || pkt.len == 0)
        return ESP_OK;

    uint8_t *buf = (uint8_t *)malloc(pkt.len + 1);
    if (!buf)
    {
        APPLOG_E("WS rx: OOM");
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;

    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret != ESP_OK)
    {
        free(buf);
        ws_client_remove(httpd_req_to_sockfd(req));
        return ret;
    }

    buf[pkt.len] = '\0';
    ws_dispatch(req, (const char *)buf);
    free(buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  WebSocket push — build full-state and display-only frames         */
/* ------------------------------------------------------------------ */

static void build_ws_payload(char *buf, size_t buf_len, uint32_t *inout_log_seq)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    uint32_t total_heap = min_heap + free_heap;
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *app1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    char ota_slot[8] = "app0";
    if (running && app1 && running->address == app1->address)
        strlcpy(ota_slot, "app1", sizeof(ota_slot));

    size_t spiffs_total = 0, spiffs_used = 0;
    esp_spiffs_info("spiffs", &spiffs_total, &spiffs_used);
    int spiffs_pct = spiffs_total ? (int)(spiffs_used * 100 / spiffs_total) : 0;

    wt_wifi_status_t wst = wt_wifi_get_status();
    float temperature_c = NAN;
    bool has_temp = read_mcu_temperature_c(&temperature_c);

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
        cJSON *dc = cJSON_AddArrayToObject(d, "digit_colors");
        for (int di = 0; di < WT_SEGD_NUM_DIGITS; di++)
        {
            cJSON *sc = cJSON_CreateArray();
            for (int si = 0; si < WT_SEGD_SEGS_PER_DIGIT; si++)
                cJSON_AddItemToArray(sc, color_to_json(snap.digit_color[di][si]));
            cJSON_AddItemToArray(dc, sc);
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

    /* ---- WiFi ---- */
    static char wifi_json[1024];
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

    /* ---- Power ---- */
    static char power_json[256];
    {
        wt_settings_t ps = wt_settings_get();
        snprintf(power_json, sizeof(power_json),
                 "{\"battery_pct\":0,\"voltage\":0.0,\"current\":0,\"source\":\"USB\","
                 "\"eta_hours\":0,\"sleep_mode\":\"%s\",\"sleep_timeout\":%d,"
                 "\"batt_alert_pct\":%d,\"ps_dim\":%s,\"ps_wifi\":%s}",
                 ps.sleep_mode, ps.sleep_timeout_s, ps.batt_alert_pct,
                 ps.ps_dim ? "true" : "false", ps.ps_wifi ? "true" : "false");
    }

    /* ---- Settings ---- */
    static char settings_json[1280];
    {
        wt_settings_t ss = wt_settings_get();
        int brt = (int)((ss.intensity * 100) / 255);
        snprintf(settings_json, sizeof(settings_json),
                 "{\"color\":\"%s\",\"brightness\":%d,"
                 "\"anim_colon\":%s,\"anim_scroll\":%s,\"anim_pulse\":%s,\"anim_transition\":%s,"
                 "\"reaction_effect\":\"%s\",\"display_mode\":\"%s\","
                 "\"display_value\":%d,\"display_text\":\"%s\","
                 "\"time_format\":%d,\"timezone\":\"%s\",\"ntp_server\":\"%s\","
                 "\"alarm1_time\":\"%s\",\"alarm1_en\":%s,"
                 "\"alarm2_time\":\"%s\",\"alarm2_en\":%s,"
                 "\"notif_type\":\"%s\",\"notif_sound\":\"%s\","
                 "\"sleep_mode\":\"%s\",\"sleep_timeout\":%d,"
                 "\"batt_alert_pct\":%d,\"ps_dim\":%s,\"ps_wifi\":%s}",
                 ss.color_hex[0] ? ss.color_hex : "#00c8ff", brt,
                 ss.colon_blink ? "true" : "false", ss.anim_scroll ? "true" : "false",
                 ss.anim_pulse ? "true" : "false", ss.anim_transition ? "true" : "false",
                 ss.reaction_effect, ss.display_mode, ss.display_value, ss.display_text,
                 ss.time_format, ss.timezone, ss.ntp_server,
                 ss.alarm1_time, ss.alarm1_en ? "true" : "false",
                 ss.alarm2_time, ss.alarm2_en ? "true" : "false",
                 ss.notif_type, ss.notif_sound, ss.sleep_mode,
                 ss.sleep_timeout_s, ss.batt_alert_pct,
                 ss.ps_dim ? "true" : "false", ss.ps_wifi ? "true" : "false");
    }

    snprintf(buf, buf_len,
             "{\"type\":\"full\","
             "\"chip_model\":\"%s\",\"cpu_cores\":%d,\"cpu_freq_mhz\":240,\"cpu_usage\":0,"
             "\"flash_size\":%u,\"flash_used_pct\":0,"
             "\"free_heap\":%lu,\"total_heap\":%lu,\"min_free_heap\":%lu,"
             "\"spiffs_used_pct\":%d,\"uptime_s\":%lld,"
             "\"app_version\":\"1.0.0\",\"build_date\":\"%s %s\","
             "\"idf_version\":\"%s\",\"ota_slot\":\"%s\","
             "\"app0_state\":\"valid\",\"app1_state\":\"empty\","
             "\"temperature\":%.1f,\"reset_reason\":\"%s\","
             "\"sta_connected\":%s,\"sta_ssid\":\"%s\","
             "\"sta_ip\":\"%s\",\"rssi\":%d,\"ap_ip\":\"%s\","
             "\"display\":%s,\"wifi\":%s,\"power\":%s,\"settings\":%s,\"logs\":%s}",
             chip_name, chip.cores, (unsigned)flash_size,
             free_heap, total_heap, min_heap,
             spiffs_pct, (long long)uptime_s,
             __DATE__, __TIME__, esp_get_idf_version(), ota_slot,
             has_temp ? temperature_c : 0.0f, reset_reason,
             wst.sta_connected ? "true" : "false", wst.sta_ssid,
             wst.sta_ip, wst.sta_rssi, wst.ap_ip,
             disp_json, wifi_json, power_json, settings_json, log_json);
}

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
                      "\"colon_color\":[%u,%u,%u],"
                      "\"digit_colors\":[",
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

    if (!append_jsonf(buf, buf_len, &offset, with_type ? "]}}" : "]}"))
    {
        goto overflow;
    }
    return;

overflow:
    snprintf(buf, buf_len, with_type ? "{\"type\":\"disp\",\"display\":null}" : "null");
}

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
            APPLOG_W("WS send failed fd=%d (%s) — removing", s_ws_fds[i], esp_err_to_name(err));
            s_ws_fds[i] = -1;
        }
    }
    xSemaphoreGive(s_ws_mutex);
}

/* ------------------------------------------------------------------ */
/*  Captive-portal — DNS hijack + OS probe HTTP handlers              */
/*                                                                     */
/*  When a device connects to the ESP32 AP, its OS sends a known HTTP */
/*  probe to check for internet access.  We intercept every DNS query */
/*  (UDP/53) and return 192.168.4.1, then serve OS-specific responses */
/*  on those probe URLs so the device shows the "Sign in" notification.*/
/* ------------------------------------------------------------------ */

#define CAPTIVE_AP_IP "192.168.4.1"
#define CAPTIVE_DNS_PORT 53
#define CAPTIVE_DNS_BUF_SIZE 512

/* Stateless DNS query handler — mutates buf in-place, sends reply.
   Called from wt_task_web after recvfrom() returns ≥12 bytes.       */
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

/* iOS / macOS — probe: GET /hotspot-detect.html
   Must NOT return 200+"Success" (that means "has internet").
   A 302 triggers the CNA (Captive Network Assistant) sheet.         */
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

/* Android / Chrome — probe: GET /generate_204
   Expects 204 for "no portal"; non-204 triggers portal notification. */
static esp_err_t handler_captive_android(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" CAPTIVE_AP_IP "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

/* Windows NCSI — probe: GET /ncsi.txt
   Content must be exactly "Microsoft NCSI".                         */
static esp_err_t handler_captive_win_ncsi(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, "Microsoft NCSI");
    return ESP_OK;
}

/* Windows connect test — probe: GET /connecttest.txt               */
static esp_err_t handler_captive_win_connect(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_sendstr(req, "Microsoft Connect Test");
    return ESP_OK;
}

/* Firefox — probe: GET /canonical.html                             */
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

/* Generic redirect — /redirect and /success.txt                    */
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

/* ------------------------------------------------------------------ */
/*  Server start                                                      */
/* ------------------------------------------------------------------ */

static void start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 17; /* 3 assets + 4 OTA + 9 captive probes + 1 WS */
    cfg.stack_size = 16384;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK)
    {
        APPLOG_E("Failed to start HTTP server");
        return;
    }

    s_http_server = server;

#define REG(m, u, h)                                                  \
    do                                                                \
    {                                                                 \
        httpd_uri_t _u = {.uri = (u), .method = (m), .handler = (h)}; \
        httpd_register_uri_handler(server, &_u);                      \
    } while (0)

    /* ── Static assets — needed for initial page load only ── */
    REG(HTTP_GET, "/", handler_root);
    REG(HTTP_GET, "/style.css", handler_style_css);
    REG(HTTP_GET, "/app.js", handler_app_js);

    /* ── OTA uploads — binary multipart, cannot go over WebSocket ── */
    REG(HTTP_POST, "/api/ota/firmware", handler_ota_firmware);
    REG(HTTP_POST, "/api/ota/index.html", handler_ota_index);
    REG(HTTP_POST, "/api/ota/style.css", handler_ota_style);
    REG(HTTP_POST, "/api/ota/app.js", handler_ota_appjs);

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

    /* ── WebSocket — ALL live data and commands go through here ── */
    {
        httpd_uri_t ws_uri = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = handler_ws,
            .is_websocket = true,
            .handle_ws_control_frames = false,
        };
        httpd_register_uri_handler(server, &ws_uri);
    }

    APPLOG_I("HTTP server started: 3 assets, 2 OTA, 9 captive, 1 WS");
}

/* ------------------------------------------------------------------ */
/*  Task                                                              */
/* ------------------------------------------------------------------ */

void wt_task_web(void *pvParameters)
{
    // APPLOG_I("---------- WEB TASK STARTED ----------");

    /* Initialise WebSocket client table and mutex */
    ws_clients_init();
    s_ws_mutex = xSemaphoreCreateMutex();
    if (!s_ws_mutex)
        APPLOG_E("Failed to create WS mutex");

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
            APPLOG_E("Captive DNS: bind() failed");
            close(dns_sock);
            dns_sock = -1;
        }
        else
        {
            APPLOG_I("Captive portal DNS running (UDP/53 → %s)", CAPTIVE_AP_IP);
        }
    }
    else
    {
        APPLOG_E("Captive DNS: socket() failed");
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
        /* 1. DNS receive — blocks at most WS_DISPLAY_INTERVAL_MS */
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

        /* 2. WS broadcast — fire once per WS_DISPLAY_INTERVAL_MS */
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
            /* Full-state frame: system, display, wifi, power, settings, logs */
            tick_cnt = 0;
            build_ws_payload(full_payload, sizeof(full_payload), &log_seq);
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
