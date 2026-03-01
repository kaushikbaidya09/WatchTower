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
// #include "esp_vfs_spiffs.h"
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
    const char *fb = "<html><body><h2>WallTick</h2>"
                     "<p>Upload index.html via OTA tab.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, fb);
    return ESP_OK;
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
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    int64_t uptime_s = esp_timer_get_time() / 1000000;

    const esp_partition_t *running = esp_ota_get_running_partition();
    char part_label[18] = "unknown";
    if (running)
        strlcpy(part_label, running->label, sizeof(part_label));

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"chip\":\"%s\",\"cores\":%d,\"revision\":%d,"
             "\"flash_kb\":%u,\"free_heap\":%lu,\"min_heap\":%lu,"
             "\"uptime_s\":%lld,\"partition\":\"%s\","
             "\"idf_ver\":\"%s\"}",
             (chip.model == CHIP_ESP32) ? "ESP32" : (chip.model == CHIP_ESP32S2) ? "ESP32-S2"
                                                : (chip.model == CHIP_ESP32S3)   ? "ESP32-S3"
                                                : (chip.model == CHIP_ESP32C3)   ? "ESP32-C3"
                                                                                 : "Unknown",
             chip.cores,
             chip.revision,
             (unsigned)(flash_size / 1024),
             free_heap,
             min_heap,
             (long long)uptime_s,
             part_label,
             esp_get_idf_version());

    RESP_JSON(req, buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/logs?seq=N                                                  */
/* ------------------------------------------------------------------ */

static esp_err_t handler_logs(httpd_req_t *req)
{
    int seq_in = 0;
    get_query_int(req, "seq", &seq_in);

    /* 8 KB should hold ~40 log entries at 200 chars each */
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

    cJSON *root = cJSON_CreateObject();

    /* STA */
    cJSON *sta = cJSON_AddObjectToObject(root, "sta");
    cJSON_AddBoolToObject(sta, "started", st.sta_started);
    cJSON_AddBoolToObject(sta, "connected", st.sta_connected);
    cJSON_AddStringToObject(sta, "ssid", st.sta_ssid);
    cJSON_AddStringToObject(sta, "ip", st.sta_ip);
    cJSON_AddNumberToObject(sta, "rssi", st.sta_rssi);
    cJSON_AddNumberToObject(sta, "active_profile", st.sta_active_profile);

    /* AP */
    cJSON *ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddBoolToObject(ap, "active", st.ap_active);
    cJSON_AddStringToObject(ap, "ssid", st.ap_ssid);
    cJSON_AddStringToObject(ap, "ip", st.ap_ip);
    cJSON_AddNumberToObject(ap, "clients", st.ap_clients);

    /* Profiles */
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
    cJSON *passj = cJSON_GetObjectItem(j, "passwd");
    const char *passwd = passj ? cJSON_GetStringValue(passj) : "";

    bool ok = ssid && wt_wifi_add_profile(ssid, passwd ? passwd : "");
    cJSON_Delete(j);

    RESP_JSON(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"full or invalid\"}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  DELETE /api/wifi/profile?idx=N                                       */
/* ------------------------------------------------------------------ */

static esp_err_t handler_wifi_del_profile(httpd_req_t *req)
{
    int idx = 0;
    if (!get_query_int(req, "idx", &idx))
    {
        RESP_ERR(req, "missing idx");
        return ESP_OK;
    }
    bool ok = wt_wifi_remove_profile(idx);
    RESP_JSON(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/wifi/connect?idx=N                                         */
/* ------------------------------------------------------------------ */

static esp_err_t handler_wifi_connect(httpd_req_t *req)
{
    int idx = 0;
    if (!get_query_int(req, "idx", &idx))
    {
        RESP_ERR(req, "missing idx");
        return ESP_OK;
    }
    bool ok = wt_wifi_connect_profile(idx);
    RESP_JSON(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  GET /api/settings                                                    */
/* ------------------------------------------------------------------ */

static esp_err_t handler_settings_get(httpd_req_t *req)
{
    wt_settings_t s = wt_settings_get();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"color_on\":{\"r\":%d,\"g\":%d,\"b\":%d},"
             "\"color_off\":{\"r\":%d,\"g\":%d,\"b\":%d},"
             "\"intensity\":%d,\"anim\":%d,"
             "\"colon_blink\":%s,"
             "\"timezone\":\"%s\"}",
             s.color_on.r, s.color_on.g, s.color_on.b,
             s.color_off.r, s.color_off.g, s.color_off.b,
             s.intensity, (int)s.anim,
             s.colon_blink ? "true" : "false",
             s.timezone);
    RESP_JSON(req, buf);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/*  POST /api/settings                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t handler_settings_set(httpd_req_t *req)
{
    char body[512];
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

    /* color_on */
    cJSON *con = cJSON_GetObjectItem(j, "color_on");
    if (con)
    {
        cJSON *r = cJSON_GetObjectItem(con, "r");
        cJSON *g = cJSON_GetObjectItem(con, "g");
        cJSON *b = cJSON_GetObjectItem(con, "b");
        if (r)
            s.color_on.r = (uint8_t)cJSON_GetNumberValue(r);
        if (g)
            s.color_on.g = (uint8_t)cJSON_GetNumberValue(g);
        if (b)
            s.color_on.b = (uint8_t)cJSON_GetNumberValue(b);
    }
    /* color_off */
    cJSON *cof = cJSON_GetObjectItem(j, "color_off");
    if (cof)
    {
        cJSON *r = cJSON_GetObjectItem(cof, "r");
        cJSON *g = cJSON_GetObjectItem(cof, "g");
        cJSON *b = cJSON_GetObjectItem(cof, "b");
        if (r)
            s.color_off.r = (uint8_t)cJSON_GetNumberValue(r);
        if (g)
            s.color_off.g = (uint8_t)cJSON_GetNumberValue(g);
        if (b)
            s.color_off.b = (uint8_t)cJSON_GetNumberValue(b);
    }
    cJSON *intj = cJSON_GetObjectItem(j, "intensity");
    if (intj)
        s.intensity = (uint8_t)cJSON_GetNumberValue(intj);

    cJSON *animj = cJSON_GetObjectItem(j, "anim");
    if (animj)
        s.anim = (wt_segd_anim_t)(int)cJSON_GetNumberValue(animj);

    cJSON *cbj = cJSON_GetObjectItem(j, "colon_blink");
    if (cbj)
        s.colon_blink = cJSON_IsTrue(cbj);

    cJSON *tzj = cJSON_GetObjectItem(j, "timezone");
    if (tzj && cJSON_IsString(tzj))
        strlcpy(s.timezone, cJSON_GetStringValue(tzj), sizeof(s.timezone));

    cJSON_Delete(j);

    bool ok = wt_settings_set(&s);
    RESP_JSON(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
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

    char buf[1024];
    int remaining = req->content_len;
    bool ota_ok = true;

    while (remaining > 0)
    {
        int to_recv = (remaining < (int)sizeof(buf))
                          ? remaining
                          : (int)sizeof(buf);
        int n = httpd_req_recv(req, buf, to_recv);
        if (n <= 0)
        {
            ota_ok = false;
            break;
        }
        if (esp_ota_write(ota_handle, buf, n) != ESP_OK)
        {
            ota_ok = false;
            break;
        }
        remaining -= n;
    }

    if (!ota_ok || esp_ota_end(ota_handle) != ESP_OK)
    {
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
            ok = false;
            break;
        }
        if (fwrite(buf, 1, n, f) != (size_t)n)
        {
            ok = false;
            break;
        }
        remaining -= n;
    }
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
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 16384;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.server_port = 8085;

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
    REG(HTTP_GET, "/api/system", handler_system);
    REG(HTTP_GET, "/api/logs", handler_logs);
    REG(HTTP_GET, "/api/wifi", handler_wifi_get);
    REG(HTTP_POST, "/api/wifi/profile", handler_wifi_add_profile);
    REG(HTTP_DELETE, "/api/wifi/profile", handler_wifi_del_profile);
    REG(HTTP_POST, "/api/wifi/connect", handler_wifi_connect);
    REG(HTTP_GET, "/api/settings", handler_settings_get);
    REG(HTTP_POST, "/api/settings", handler_settings_set);
    REG(HTTP_POST, "/api/ota/firmware", handler_ota_firmware);
    REG(HTTP_POST, "/api/ota/webapp", handler_ota_webapp);
    REG(HTTP_POST, "/api/reboot", handler_reboot);

#undef REG

    APPLOG_I("HTTP server started on port '%d'", cfg.server_port);
}

/* ------------------------------------------------------------------ */
/*  Task                                                                 */
/* ------------------------------------------------------------------ */

void wt_task_web(void *pvParameters)
{
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
