/*
 * pt_app — Wi-Fi + HTTP + transport glue. MVP scope: brings everything
 * up and serves /api/status. Print, cut, feed, SPA, AP onboarding land
 * in follow-up issues without changing this skeleton.
 */

#include "pt_app.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "pt_protocol.h"
#include "pt_session.h"
#include "pt_transport.h"
#include "pt_transport_mock.h"
#include "pt_transport_usb_host.h"

static const char *TAG = "pt_app";

/* ------------------------------------------------------------ Wi-Fi --- */

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_MAX_RETRIES   8

static EventGroupHandle_t s_wifi_event_group;
static int                s_retry_count;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "wifi reconnect %d", s_retry_count);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_sta_up(const char *ssid, const char *password)
{
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t sta = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)sta.sta.ssid,     ssid,     sizeof sta.sta.ssid - 1);
    strncpy((char *)sta.sta.password, password, sizeof sta.sta.password - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

/* --------------------------------------------------------- transport --- */

static pt_transport_t            s_transport;
static pt_transport_mock_t       s_mock;
static pt_transport_usb_host_t  *s_usb;

static void transport_up(const pt_app_config_t *cfg)
{
    if (cfg->use_usb_host) {
        uint32_t to = cfg->usb_connect_timeout_ms ? cfg->usb_connect_timeout_ms : 10000;
        s_usb = pt_transport_usb_host_open(to);
        if (s_usb) {
            s_transport = pt_transport_usb_host_transport(s_usb);
            ESP_LOGI(TAG, "transport: usb_host (PT-* attached)");
            return;
        }
        ESP_LOGW(TAG, "USB host open failed — falling back to mock");
    }
    s_transport = pt_transport_mock_init(&s_mock);
    ESP_LOGI(TAG, "transport: mock (no real printer)");
}

/* ----------------------------------------------------------- HTTP API --- */

static const char *err_kind(pt_err_t e)
{
    switch (e) {
    case PT_OK: return "ok";
    case PT_ERR_NO_MEDIA:       return "no_media";
    case PT_ERR_COVER_OPEN:     return "cover_open";
    case PT_ERR_CUTTER_JAM:     return "cutter_jam";
    case PT_ERR_OVERHEAT:       return "overheat";
    case PT_ERR_REPLACE_MEDIA:  return "replace_media";
    case PT_ERR_WEAK_BATTERY:   return "weak_battery";
    case PT_ERR_HIGH_VOLTAGE:   return "high_voltage";
    case PT_ERR_MEDIA_MISMATCH: return "media_mismatch";
    case PT_ERR_TIMEOUT:        return "timeout";
    case PT_ERR_TRANSPORT:      return "transport";
    default:                    return "internal";
    }
}

static esp_err_t api_status(httpd_req_t *req)
{
    pt_status_t st;
    pt_err_t err = pt_session_query_status(&s_transport, &st, NULL);

    char body[384];
    int  n;
    if (err != PT_OK) {
        n = snprintf(body, sizeof body,
                     "{\"ok\":false,\"error\":\"%s\"}", err_kind(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
    } else {
        n = snprintf(body, sizeof body,
            "{\"ok\":true,"
            "\"model\":%u,"
            "\"media_width_mm\":%u,"
            "\"media_type\":%u,"
            "\"tape_color_id\":%u,"
            "\"text_color_id\":%u,"
            "\"error1\":%u,"
            "\"error2\":%u,"
            "\"status_type\":%u,"
            "\"phase_type\":%u}",
            (unsigned)st.model,
            (unsigned)st.media_width_mm,
            (unsigned)st.media_type,
            (unsigned)st.tape_color_id,
            (unsigned)st.text_color_id,
            (unsigned)st.error1,
            (unsigned)st.error2,
            (unsigned)st.status_type,
            (unsigned)st.phase_type);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* Index page placeholder until the SPA lands (pt700-x4i). */
static esp_err_t api_index(httpd_req_t *req)
{
    static const char body[] =
        "<!doctype html><meta charset=utf-8>"
        "<title>pt700</title>"
        "<h1>pt700</h1>"
        "<p>API up. SPA pending — see "
        "<code>GET /api/status</code> for now.</p>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, body, sizeof body - 1);
}

static httpd_handle_t s_http;

static esp_err_t http_up(uint16_t port)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = port ? port : 80;
    cfg.lru_purge_enable = true;

    if (httpd_start(&s_http, &cfg) != ESP_OK) return ESP_FAIL;

    static const httpd_uri_t status_route = {
        .uri = "/api/status", .method = HTTP_GET,
        .handler = api_status, .user_ctx = NULL,
    };
    static const httpd_uri_t index_route = {
        .uri = "/", .method = HTTP_GET,
        .handler = api_index, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_http, &status_route);
    httpd_register_uri_handler(s_http, &index_route);
    return ESP_OK;
}

/* --------------------------------------------------------- public ---- */

esp_err_t pt_app_run(const pt_app_config_t *cfg)
{
    if (!cfg || !cfg->wifi_ssid || !cfg->wifi_password) return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_LOGI(TAG, "wifi: connecting to %s", cfg->wifi_ssid);
    if (wifi_sta_up(cfg->wifi_ssid, cfg->wifi_password) != ESP_OK) {
        ESP_LOGE(TAG, "wifi: failed");
        return ESP_FAIL;
    }

    transport_up(cfg);

    if (http_up(cfg->http_port) != ESP_OK) {
        ESP_LOGE(TAG, "http: failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "http: serving on port %u", cfg->http_port ? cfg->http_port : 80);
    return ESP_OK;
}
