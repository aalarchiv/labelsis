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
#include "mdns.h"
#include "nvs_flash.h"

#include "nvs.h"

#include "pt_protocol.h"
#include "pt_session.h"
#include "pt_transport.h"
#include "pt_transport_mock.h"
#include "pt_transport_usb_host.h"

static const char *TAG = "pt_app";

/* ---------------------------------------------------- Wi-Fi creds (NVS) -- */

/* Persist Wi-Fi creds in NVS so first-boot provisioning (whether via
 * compiled-in cfg or AP-mode onboarding) is durable across reboots.
 * Buffers sized for the IEEE 802.11 SSID (32 bytes + NUL) and the
 * WPA2 PSK string form (64 chars + NUL). */
#define CREDS_NS    "pt_app"
#define CREDS_SSID  "ssid"
#define CREDS_PASS  "pass"

typedef struct {
    char ssid[33];
    char password[65];
} wifi_creds_t;

static esp_err_t creds_load(wifi_creds_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CREDS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t sz = sizeof out->ssid;
    err = nvs_get_str(h, CREDS_SSID, out->ssid, &sz);
    if (err == ESP_OK) {
        sz = sizeof out->password;
        err = nvs_get_str(h, CREDS_PASS, out->password, &sz);
    }
    nvs_close(h);
    return err;
}

static esp_err_t creds_save(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, CREDS_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, CREDS_PASS, password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

__attribute__((unused))
static esp_err_t creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CREDS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, CREDS_SSID);
    nvs_erase_key(h, CREDS_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

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

/* Read a header value as a boolean (case-insensitive "true"/"false"),
 * leaving *dst untouched if the header isn't present. */
static void hdr_bool(httpd_req_t *req, const char *name, bool *dst)
{
    char v[16];
    if (httpd_req_get_hdr_value_str(req, name, v, sizeof v) != ESP_OK) return;
    if (strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0) *dst = true;
    else if (strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0) *dst = false;
}

/* GET /api/info — geometry + identity + non-printable margins. */
static esp_err_t api_info(httpd_req_t *req)
{
    pt_status_t st;
    pt_err_t err = pt_session_query_status(&s_transport, &st, NULL);
    if (err != PT_OK) {
        char body[96];
        int n = snprintf(body, sizeof body,
                         "{\"ok\":false,\"error\":\"%s\"}", err_kind(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, body, n);
    }

    pt_tape_geometry_t g = {0};
    bool have_geom = (pt_tape_geometry_tze(st.media_width_mm, &g) == PT_OK);
    /* Per-side non-printable margin in 180-dpi dots: half the
     * difference between physical tape width and the print area. */
    unsigned offside = have_geom
        ? (unsigned)((g.tape_width_dots - g.print_pins) / 2u) : 0;

    char body[384];
    int  n = snprintf(body, sizeof body,
        "{\"ok\":true,"
        "\"model\":%u,"
        "\"media_width_mm\":%u,"
        "\"media_type\":%u,"
        "\"tape_color_id\":%u,"
        "\"text_color_id\":%u,"
        "\"geometry\":{"
            "\"head_pins\":%u,"
            "\"print_pins\":%u,"
            "\"tape_width_dots\":%u,"
            "\"left_margin_pins\":%u,"
            "\"right_margin_pins\":%u,"
            "\"non_printable_dots_per_side\":%u}}",
        (unsigned)st.model,
        (unsigned)st.media_width_mm,
        (unsigned)st.media_type,
        (unsigned)st.tape_color_id,
        (unsigned)st.text_color_id,
        (unsigned)(have_geom ? g.total_pins : 0),
        (unsigned)(have_geom ? g.print_pins : 0),
        (unsigned)(have_geom ? g.tape_width_dots : 0),
        (unsigned)(have_geom ? g.left_margin_pins : 0),
        (unsigned)(have_geom ? g.right_margin_pins : 0),
        offside);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

/* POST /api/feed — advance the tape by `dots` (default 100) without
 * printing. POST /api/cut — issue a single cut at the current head
 * position. Both build a tiny one-page print job containing a single
 * zero raster row, with the relevant flags set, so the printer's
 * normal feed/cut path runs. */
static esp_err_t feed_or_cut(httpd_req_t *req, bool do_cut, uint16_t dots)
{
    pt_session_options_t opts;
    pt_session_options_default(&opts);
    opts.compression  = PT_COMPRESSION_NONE;  /* avoid PackBits round-trip */
    opts.auto_cut     = do_cut;
    opts.no_chain_print = true;
    opts.margin_dots  = do_cut ? 14 : dots;   /* feed = use margin to advance */

    /* Single zero raster row = printable nothing; printer still feeds the
     * configured margin and (optionally) cuts. */
    static const uint8_t zero_row[16] = {0};
    pt_err_t err = pt_session_print_raster(&s_transport, zero_row, 1,
                                           0 /* trust loaded width */, &opts);

    char body[96];
    int  n;
    if (err == PT_OK) {
        n = snprintf(body, sizeof body, "{\"ok\":true}");
    } else {
        n = snprintf(body, sizeof body,
                     "{\"ok\":false,\"error\":\"%s\"}", err_kind(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n);
}

static esp_err_t api_cut(httpd_req_t *req)  { return feed_or_cut(req, true, 0); }

static esp_err_t api_feed(httpd_req_t *req)
{
    /* Optional ?dots=N query parameter; default 100 (~14 mm @ 180 dpi). */
    uint16_t dots = 100;
    char query[32];
    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "dots", val, sizeof val) == ESP_OK) {
            int v = atoi(val);
            if (v > 0 && v < 2048) dots = (uint16_t)v;
        }
    }
    return feed_or_cut(req, false, dots);
}

/* POST /api/print
 * Body  : raw 1-bit raster, n_rows × 16 bytes (the layout
 *         pt_bitmap_to_raster produces).
 * Headers (all optional):
 *   X-Tape-Width-Mm    require this width or fail (default: trust loaded)
 *   X-Auto-Cut         "true"|"false"   default true
 *   X-Chain            "true"|"false"   default false; true also disables
 *                                       auto-cut (PT-P700 specific, see
 *                                       pt_send --chain)
 *   X-Mirror           "true"|"false"   default false
 *   X-No-Compression   "true"|"false"   default false (= TIFF/PackBits)
 *   X-Margin-Dots      integer          default 14 (~ 2 mm @ 180 dpi)
 * Reply : application/json
 *   ok=true  : {"ok":true,"rows":N}
 *   ok=false : {"ok":false,"error":"<kind>"}    + 4xx/5xx status
 */
#define MAX_PRINT_BODY (128 * 1024)

static esp_err_t api_print(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total % 16 != 0 || total > MAX_PRINT_BODY) {
        httpd_resp_set_status(req, total > MAX_PRINT_BODY
                              ? "413 Payload Too Large" : "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        const char *what = (total <= 0)            ? "missing_body"
                         : (total > MAX_PRINT_BODY) ? "too_large"
                                                    : "length_not_aligned_16";
        char body[96];
        int n = snprintf(body, sizeof body,
                         "{\"ok\":false,\"error\":\"%s\"}", what);
        return httpd_resp_send(req, body, n);
    }

    uint8_t *buf = malloc((size_t)total);
    if (!buf) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }

    int rcvd = 0;
    while (rcvd < total) {
        int n = httpd_req_recv(req, (char *)(buf + rcvd), total - rcvd);
        if (n <= 0) {
            free(buf);
            return ESP_FAIL;  /* connection closed */
        }
        rcvd += n;
    }

    pt_session_options_t opts;
    pt_session_options_default(&opts);

    char hbuf[16];
    hdr_bool(req, "X-Auto-Cut", &opts.auto_cut);
    hdr_bool(req, "X-Mirror",   &opts.mirror_print);
    /* --chain implies no_chain_print=false AND auto_cut=false on PT-P700
     * (see pt_send --chain). Apply the same coupling here. */
    bool chain = false;
    hdr_bool(req, "X-Chain", &chain);
    if (chain) { opts.no_chain_print = false; opts.auto_cut = false; }

    bool no_compress = false;
    hdr_bool(req, "X-No-Compression", &no_compress);
    if (no_compress) opts.compression = PT_COMPRESSION_NONE;

    if (httpd_req_get_hdr_value_str(req, "X-Margin-Dots",
                                    hbuf, sizeof hbuf) == ESP_OK) {
        opts.margin_dots = (uint16_t)atoi(hbuf);
    }
    uint8_t width = 0;
    if (httpd_req_get_hdr_value_str(req, "X-Tape-Width-Mm",
                                    hbuf, sizeof hbuf) == ESP_OK) {
        width = (uint8_t)atoi(hbuf);
    }

    size_t   n_rows = (size_t)total / 16;
    pt_err_t err = pt_session_print_raster(&s_transport, buf,
                                           n_rows, width, &opts);
    free(buf);

    char rbody[128];
    int  rlen;
    if (err == PT_OK) {
        rlen = snprintf(rbody, sizeof rbody,
                        "{\"ok\":true,\"rows\":%u}", (unsigned)n_rows);
    } else {
        rlen = snprintf(rbody, sizeof rbody,
                        "{\"ok\":false,\"error\":\"%s\"}", err_kind(err));
        switch (err) {
        case PT_ERR_NO_MEDIA:
        case PT_ERR_REPLACE_MEDIA:
        case PT_ERR_COVER_OPEN:
        case PT_ERR_MEDIA_MISMATCH:
        case PT_ERR_CUTTER_JAM:
            httpd_resp_set_status(req, "409 Conflict");
            break;
        case PT_ERR_OVERHEAT:
        case PT_ERR_WEAK_BATTERY:
        case PT_ERR_HIGH_VOLTAGE:
            httpd_resp_set_status(req, "503 Service Unavailable");
            break;
        case PT_ERR_TIMEOUT:
            httpd_resp_set_status(req, "504 Gateway Timeout");
            break;
        default:
            httpd_resp_set_status(req, "500 Internal Server Error");
        }
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, rbody, rlen);
}

/* Single-file SPA embedded via EMBED_TXTFILES in CMakeLists.txt — the
 * label designer. Inline CSS + JS so we don't need LittleFS yet; the
 * file is < 10 KB and lives in flash next to the code. */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[]   asm("_binary_index_html_end");

static esp_err_t api_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start,
                           index_html_end - index_html_start);
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
    static const httpd_uri_t info_route = {
        .uri = "/api/info", .method = HTTP_GET,
        .handler = api_info, .user_ctx = NULL,
    };
    static const httpd_uri_t print_route = {
        .uri = "/api/print", .method = HTTP_POST,
        .handler = api_print, .user_ctx = NULL,
    };
    static const httpd_uri_t cut_route = {
        .uri = "/api/cut", .method = HTTP_POST,
        .handler = api_cut, .user_ctx = NULL,
    };
    static const httpd_uri_t feed_route = {
        .uri = "/api/feed", .method = HTTP_POST,
        .handler = api_feed, .user_ctx = NULL,
    };
    static const httpd_uri_t index_route = {
        .uri = "/", .method = HTTP_GET,
        .handler = api_index, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_http, &status_route);
    httpd_register_uri_handler(s_http, &info_route);
    httpd_register_uri_handler(s_http, &print_route);
    httpd_register_uri_handler(s_http, &cut_route);
    httpd_register_uri_handler(s_http, &feed_route);
    httpd_register_uri_handler(s_http, &index_route);
    return ESP_OK;
}

/* ------------------------------------------------------------- mDNS --- */

static esp_err_t mdns_up(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    mdns_hostname_set("pt700");
    mdns_instance_name_set("pt700 print server");
    /* Advertise the HTTP service on the configured port. */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    return ESP_OK;
}

/* --------------------------------------------------------- public ---- */

esp_err_t pt_app_run(const pt_app_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Resolve Wi-Fi creds: NVS wins if present, else fall back to the
     * compiled-in cfg (and persist on first successful connect). */
    wifi_creds_t stored = {0};
    bool have_stored = (creds_load(&stored) == ESP_OK
                        && stored.ssid[0] != '\0');
    bool have_cfg    = (cfg->wifi_ssid && cfg->wifi_password
                        && cfg->wifi_ssid[0] && cfg->wifi_password[0]);

    const char *ssid     = have_stored ? stored.ssid     : have_cfg ? cfg->wifi_ssid     : NULL;
    const char *password = have_stored ? stored.password : have_cfg ? cfg->wifi_password : NULL;

    if (!ssid) {
        ESP_LOGE(TAG, "wifi: no creds (NVS empty, no compiled-in fallback)");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "wifi: connecting to %s (%s)",
             ssid, have_stored ? "from NVS" : "from cfg");
    if (wifi_sta_up(ssid, password) != ESP_OK) {
        ESP_LOGE(TAG, "wifi: failed");
        return ESP_FAIL;
    }
    if (!have_stored) {
        if (creds_save(ssid, password) == ESP_OK) {
            ESP_LOGI(TAG, "wifi: persisted creds to NVS");
        } else {
            ESP_LOGW(TAG, "wifi: failed to persist creds to NVS");
        }
    }

    transport_up(cfg);

    if (mdns_up() == ESP_OK) {
        ESP_LOGI(TAG, "mdns: pt700.local up");
    } else {
        ESP_LOGW(TAG, "mdns: init failed (use the IP address instead)");
    }

    if (http_up(cfg->http_port) != ESP_OK) {
        ESP_LOGE(TAG, "http: failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "http: serving on port %u", cfg->http_port ? cfg->http_port : 80);
    return ESP_OK;
}
