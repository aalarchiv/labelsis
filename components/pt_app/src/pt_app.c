/*
 * pt_app — Wi-Fi + HTTP + transport glue. Owns:
 *   - NVS-persisted Wi-Fi creds with first-boot fallback to compiled-in
 *   - SoftAP onboarding ("pt700-setup" + setup page) when no creds
 *   - STA bring-up + transport open + mDNS + the HTTP API
 *   - long-press GPIO that wipes creds and re-enters AP mode
 */

#include "pt_app.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

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

/* ----------------------------------------------------- reset button -- */

#define RESET_HOLD_MS  5000   /* duration to qualify as a long press */
#define RESET_POLL_MS  50     /* GPIO sample period */

/* Polls an active-low input. Holding it for RESET_HOLD_MS clears creds
 * and reboots — next boot enters AP-mode onboarding. We sample rather
 * than use an ISR because debounce + duration tracking is simpler in a
 * task, and the work is trivial (20 reads/s, costs ~nothing). */
static void reset_button_task(void *arg)
{
    int gpio = (int)(intptr_t)arg;
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&gc) != ESP_OK) {
        ESP_LOGW(TAG, "reset: gpio %d config failed — disabling", gpio);
        vTaskDelete(NULL);
    }

    TickType_t pressed_at = 0;
    while (1) {
        bool       down = (gpio_get_level(gpio) == 0);
        TickType_t now  = xTaskGetTickCount();
        if (down && pressed_at == 0) {
            pressed_at = now;
        } else if (!down) {
            pressed_at = 0;
        } else if ((now - pressed_at) >= pdMS_TO_TICKS(RESET_HOLD_MS)) {
            ESP_LOGW(TAG, "reset: long press on gpio %d — clearing creds", gpio);
            creds_clear();
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(RESET_POLL_MS));
    }
}

static void reset_button_up(int gpio)
{
    if (gpio < 0) {
        ESP_LOGI(TAG, "reset: disabled (gpio < 0)");
        return;
    }
    BaseType_t r = xTaskCreate(reset_button_task, "rstbtn", 2048,
                               (void *)(intptr_t)gpio,
                               tskIDLE_PRIORITY + 1, NULL);
    if (r != pdPASS) ESP_LOGW(TAG, "reset: task create failed");
    else             ESP_LOGI(TAG, "reset: gpio %d, hold %d ms", gpio, RESET_HOLD_MS);
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
        /* APSTA onboarding mode brings up the radio without an STA
         * SSID configured — skip auto-connect when there's nothing
         * to connect to. */
        wifi_config_t cur;
        if (esp_wifi_get_config(WIFI_IF_STA, &cur) == ESP_OK
            && cur.sta.ssid[0] != 0) {
            esp_wifi_connect();
        }
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
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "ap: client joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "ap: client left");
    }
}

/* One-shot Wi-Fi stack init: netif, event loop, default STA+AP netifs,
 * wifi_init, our shared event handler. Both wifi_sta_up and wifi_ap_up
 * call this; subsequent calls no-op so APSTA mode (used by the AP-mode
 * setup page's network scanner) doesn't double-init anything. */
static esp_err_t wifi_init_once(void)
{
    static bool inited = false;
    if (inited) return ESP_OK;

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    inited = true;
    return ESP_OK;
}

static esp_err_t wifi_sta_up(const char *ssid, const char *password)
{
    if (wifi_init_once() != ESP_OK) return ESP_FAIL;

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

/* Bring up an open SoftAP for first-boot onboarding on 192.168.4.1
 * (esp-netif default). APSTA mode lets the setup page scan nearby
 * networks without tearing the AP down. */
static esp_err_t wifi_ap_up(const char *ssid)
{
    if (wifi_init_once() != ESP_OK) return ESP_FAIL;

    /* If wifi_sta_up was called and failed, the radio is already
     * started in STA mode — stop before switching modes. */
    esp_wifi_stop();

    wifi_config_t ap = {
        .ap = {
            .ssid_len       = (uint8_t)strlen(ssid),
            .channel        = 6,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)ap.ap.ssid, ssid, sizeof ap.ap.ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

/* --------------------------------------------------------- transport --- */

static pt_transport_t            s_transport;
static pt_transport_mock_t       s_mock;
static pt_transport_usb_host_t  *s_usb;
static const char               *s_transport_name = "none";

static void transport_up(const pt_app_config_t *cfg)
{
    if (cfg->use_usb_host) {
        uint32_t to = cfg->usb_connect_timeout_ms ? cfg->usb_connect_timeout_ms : 10000;
        s_usb = pt_transport_usb_host_open(to);
        if (s_usb) {
            s_transport      = pt_transport_usb_host_transport(s_usb);
            s_transport_name = "usb_host";
            ESP_LOGI(TAG, "transport: usb_host (PT-* attached)");
            return;
        }
        if (pt_transport_usb_host_plite_seen()) {
            s_transport      = pt_transport_mock_init(&s_mock);
            s_transport_name = "plite";
            ESP_LOGW(TAG, "transport: PT-* in P-Lite mode (no printer interface)");
            return;
        }
        ESP_LOGW(TAG, "USB host open failed — falling back to mock");
    }
    s_transport      = pt_transport_mock_init(&s_mock);
    s_transport_name = "mock";
    ESP_LOGI(TAG, "transport: mock (no real printer)");
}

/* ----------------------------------------------------------- HTTP API --- */

/* Permissive CORS so the SPA can be developed and tested from any
 * origin (e.g. file://, localhost dev server) hitting a real device
 * over the LAN. Same security posture as before: the API has no auth,
 * anyone reachable on the LAN could already drive it from a same-
 * origin tool — CORS just lets browser pages do it too. */
static void cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin",  "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age",       "3600");
}

/* OPTIONS catch-all — answers CORS preflight for any path with the
 * shared CORS headers. Browsers fire this before any non-simple
 * request (Content-Type: application/json, custom X-* headers, etc.). */
static esp_err_t cors_preflight(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

/* Serializes whole pt_session operations across HTTP worker threads.
 * pt_transport already mutexes individual send/recv pairs, but a print
 * is many recvs in a row (wait_print_done loops on status messages
 * looking for phase=printing) — without a higher-level lock, a /api/
 * status poll fired by the SPA's 3 s timer can slot between two of
 * those recvs and consume the printer's lone phase=printing message,
 * making the print's wait loop time out even though the print is fine.
 *
 * Take this mutex around any pt_session call. Polls and prints
 * serialize cleanly; the SPA briefly stops refreshing during a print,
 * which is exactly what we want anyway. */
static SemaphoreHandle_t s_session_mutex;

#define SESSION_LOCK(timeout_ms) \
    (xSemaphoreTake(s_session_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE)
#define SESSION_UNLOCK() \
    xSemaphoreGive(s_session_mutex)

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
    cors_headers(req);
    /* Short wait — a status poll that races with a long print should
     * fail fast as "busy" rather than holding the connection while the
     * print finishes. */
    if (!SESSION_LOCK(500)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"transport\":\"busy\",\"error\":\"busy\"}");
    }
    pt_status_t st;
    pt_err_t err = pt_session_query_status(&s_transport, &st, NULL);
    SESSION_UNLOCK();

    char body[384];
    int  n;
    if (err != PT_OK) {
        n = snprintf(body, sizeof body,
                     "{\"ok\":false,\"transport\":\"%s\",\"error\":\"%s\"}",
                     s_transport_name, err_kind(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
    } else {
        n = snprintf(body, sizeof body,
            "{\"ok\":true,"
            "\"transport\":\"%s\","
            "\"model\":%u,"
            "\"media_width_mm\":%u,"
            "\"media_type\":%u,"
            "\"tape_color_id\":%u,"
            "\"text_color_id\":%u,"
            "\"error1\":%u,"
            "\"error2\":%u,"
            "\"status_type\":%u,"
            "\"phase_type\":%u}",
            s_transport_name,
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
    cors_headers(req);
    if (!SESSION_LOCK(500)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req,
            "{\"ok\":false,\"transport\":\"busy\",\"error\":\"busy\"}");
    }
    pt_status_t st;
    pt_err_t err = pt_session_query_status(&s_transport, &st, NULL);
    SESSION_UNLOCK();
    if (err != PT_OK) {
        char body[128];
        int n = snprintf(body, sizeof body,
                         "{\"ok\":false,\"transport\":\"%s\",\"error\":\"%s\"}",
                         s_transport_name, err_kind(err));
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

    char body[416];
    int  n = snprintf(body, sizeof body,
        "{\"ok\":true,"
        "\"transport\":\"%s\","
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
        s_transport_name,
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
    cors_headers(req);
    /* Wait the full session-lock window — feed/cut still go through
     * pt_session_print_raster (with a one-row zero job), so they need
     * exclusive printer access for the duration. */
    if (!SESSION_LOCK(150000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"busy\"}");
    }

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
    SESSION_UNLOCK();

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
    cors_headers(req);
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

    /* Hold the lock across the entire job (encode + raster send +
     * wait_print_done). Long enough for a 1 m label at PT-P700's
     * ~30 mm/s — print_timeout_ms (default 120 s) plus margin. */
    if (!SESSION_LOCK(150000)) {
        free(buf);
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"busy\"}");
    }
    pt_err_t err = pt_session_print_raster(&s_transport, buf,
                                           n_rows, width, &opts);
    SESSION_UNLOCK();
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
    cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start,
                           index_html_end - index_html_start);
}

static httpd_handle_t s_http;

/* Forward decls: api_scan and api_setup are defined further down with
 * the AP-onboarding handlers, but the STA-mode http_up registers them
 * too so the SPA's Wi-Fi tab can re-provision in place. */
static esp_err_t api_scan(httpd_req_t *req);
static esp_err_t api_setup(httpd_req_t *req);

static esp_err_t http_up(uint16_t port)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = port ? port : 80;
    cfg.lru_purge_enable = true;
    /* Wildcard matching enables the OPTIONS catch-all below; specific
     * routes still match exactly. */
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    /* Default is 8; we register 9 (status, info, print, cut, feed,
     * scan, setup, index, OPTIONS catch-all) and want headroom. */
    cfg.max_uri_handlers = 16;

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
    static const httpd_uri_t scan_route = {
        .uri = "/api/scan", .method = HTTP_GET,
        .handler = api_scan, .user_ctx = NULL,
    };
    static const httpd_uri_t setup_route = {
        .uri = "/api/setup", .method = HTTP_POST,
        .handler = api_setup, .user_ctx = NULL,
    };
    static const httpd_uri_t index_route = {
        .uri = "/", .method = HTTP_GET,
        .handler = api_index, .user_ctx = NULL,
    };
    static const httpd_uri_t cors_route = {
        .uri = "/*", .method = HTTP_OPTIONS,
        .handler = cors_preflight, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_http, &status_route);
    httpd_register_uri_handler(s_http, &info_route);
    httpd_register_uri_handler(s_http, &print_route);
    httpd_register_uri_handler(s_http, &cut_route);
    httpd_register_uri_handler(s_http, &feed_route);
    httpd_register_uri_handler(s_http, &scan_route);
    httpd_register_uri_handler(s_http, &setup_route);
    httpd_register_uri_handler(s_http, &index_route);
    httpd_register_uri_handler(s_http, &cors_route);
    return ESP_OK;
}

/* ------------------------------------------------------ AP onboarding -- */

#define AP_SETUP_SSID "pt700-setup"

extern const char setup_html_start[] asm("_binary_setup_html_start");
extern const char setup_html_end[]   asm("_binary_setup_html_end");

/* Catch-all GET handler — serves the setup page for /, captive-portal
 * detection URLs (Apple/Android/Microsoft probes), and any unknown
 * path. With no DNS hijack the popup behaviour is best-effort: probes
 * see non-success HTML and most phones surface a "sign in" notice. */
static esp_err_t ap_setup_page(httpd_req_t *req)
{
    cors_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, setup_html_start,
                           setup_html_end - setup_html_start);
}

/* Append `s` (`n` bytes) to the response stream as a JSON string
 * literal — RFC 8259 escaping for ", \, and any byte < 0x20 or >=
 * 0x7f. SSIDs are arbitrary bytes; non-printables become \uXXXX. */
static void send_json_string(httpd_req_t *req, const uint8_t *s, size_t n)
{
    httpd_resp_send_chunk(req, "\"", 1);
    char buf[16];
    for (size_t i = 0; i < n && s[i]; i++) {
        uint8_t c = s[i];
        int  blen;
        if      (c == '"' || c == '\\') { buf[0] = '\\'; buf[1] = (char)c; blen = 2; }
        else if (c >= 0x20 && c < 0x7f) { buf[0] = (char)c;                blen = 1; }
        else                            { blen = snprintf(buf, sizeof buf, "\\u%04x", c); }
        httpd_resp_send_chunk(req, buf, blen);
    }
    httpd_resp_send_chunk(req, "\"", 1);
}

/* GET /api/scan — blocking scan on the STA interface (APSTA mode is
 * up). Returns up to 30 best-RSSI APs; the page dedupes by SSID. */
static esp_err_t api_scan(httpd_req_t *req)
{
    cors_headers(req);
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"scan_failed\"}");
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    wifi_ap_record_t *recs = n ? calloc(n, sizeof *recs) : NULL;
    if (n && !recs) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"oom\"}");
    }
    if (n) esp_wifi_scan_get_ap_records(&n, recs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"ok\":true,\"networks\":[", 23);
    for (uint16_t i = 0; i < n; i++) {
        if (i) httpd_resp_send_chunk(req, ",", 1);
        httpd_resp_send_chunk(req, "{\"ssid\":", 8);
        send_json_string(req, recs[i].ssid,
            strnlen((const char *)recs[i].ssid, sizeof recs[i].ssid));
        char tail[64];
        int  len = snprintf(tail, sizeof tail,
                            ",\"rssi\":%d,\"auth\":%u}",
                            recs[i].rssi, (unsigned)recs[i].authmode);
        httpd_resp_send_chunk(req, tail, len);
    }
    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    free(recs);
    return ESP_OK;
}

/* Hand-parse "key":"value" out of body. Honours \" and \\ escapes
 * inside the value; everything else is taken byte-for-byte. The
 * setup body is small (< 256 bytes) and known-shape, so pulling in
 * cJSON for two strings isn't worth its .text. */
static bool extract_str(const char *body, const char *key,
                        char *out, size_t cap)
{
    char pat[64];
    int  patlen = snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += patlen;
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; }
        else                    { out[i++] = *p++; }
    }
    if (*p != '"') return false;  /* unterminated or truncated */
    out[i] = '\0';
    return true;
}

/* POST /api/setup body: {"ssid":"...","password":"..."}. Persists creds
 * to NVS, replies, then reboots so the next boot enters STA mode. */
static esp_err_t api_setup(httpd_req_t *req)
{
    cors_headers(req);
    char body[300];
    int  total = req->content_len;
    if (total <= 0 || total >= (int)sizeof body) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad_body\"}");
    }
    int rcvd = 0;
    while (rcvd < total) {
        int n = httpd_req_recv(req, body + rcvd, total - rcvd);
        if (n <= 0) return ESP_FAIL;
        rcvd += n;
    }
    body[rcvd] = '\0';

    char ssid[33] = {0}, password[65] = {0};
    bool got_ssid = extract_str(body, "ssid",     ssid,     sizeof ssid);
    extract_str(body, "password", password, sizeof password);  /* may be empty for open APs */
    if (!got_ssid || ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing_ssid\"}");
    }

    if (creds_save(ssid, password) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"nvs_save\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    ESP_LOGI(TAG, "wifi: creds saved (ssid=%s) — rebooting", ssid);
    /* Let the response FIN onto the wire before we drop the AP. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t http_ap_up(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;

    if (httpd_start(&s_http, &cfg) != ESP_OK) return ESP_FAIL;

    static const httpd_uri_t scan_route = {
        .uri = "/api/scan", .method = HTTP_GET,
        .handler = api_scan, .user_ctx = NULL,
    };
    static const httpd_uri_t setup_route = {
        .uri = "/api/setup", .method = HTTP_POST,
        .handler = api_setup, .user_ctx = NULL,
    };
    /* Catch-all serves the setup page for any GET (incl. captive-
     * portal probe URLs). Registered last; specific routes win. */
    static const httpd_uri_t catch_all = {
        .uri = "/*", .method = HTTP_GET,
        .handler = ap_setup_page, .user_ctx = NULL,
    };
    static const httpd_uri_t cors_route = {
        .uri = "/*", .method = HTTP_OPTIONS,
        .handler = cors_preflight, .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_http, &scan_route);
    httpd_register_uri_handler(s_http, &setup_route);
    httpd_register_uri_handler(s_http, &catch_all);
    httpd_register_uri_handler(s_http, &cors_route);
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

    s_session_mutex = xSemaphoreCreateMutex();
    if (!s_session_mutex) return ESP_ERR_NO_MEM;

    /* Start the reset-button watcher first so a stuck button can rescue
     * a board that would otherwise wedge in a connect/AP loop. */
    reset_button_up(cfg->reset_gpio_num);

    /* Resolve Wi-Fi creds: NVS wins if present, else fall back to the
     * compiled-in cfg (and persist on first successful connect). */
    wifi_creds_t stored = {0};
    bool have_stored = (creds_load(&stored) == ESP_OK
                        && stored.ssid[0] != '\0');
    bool have_cfg    = (cfg->wifi_ssid && cfg->wifi_password
                        && cfg->wifi_ssid[0] && cfg->wifi_password[0]);

    const char *ssid     = have_stored ? stored.ssid     : have_cfg ? cfg->wifi_ssid     : NULL;
    const char *password = have_stored ? stored.password : have_cfg ? cfg->wifi_password : NULL;

    bool ap_mode = (ssid == NULL);
    if (ssid) {
        ESP_LOGI(TAG, "wifi: connecting to %s (%s)",
                 ssid, have_stored ? "from NVS" : "from cfg");
        if (wifi_sta_up(ssid, password) != ESP_OK) {
            ESP_LOGW(TAG, "wifi: STA failed — entering AP onboarding");
            ap_mode = true;
        } else if (!have_stored) {
            if (creds_save(ssid, password) == ESP_OK) {
                ESP_LOGI(TAG, "wifi: persisted creds to NVS");
            } else {
                ESP_LOGW(TAG, "wifi: failed to persist creds to NVS");
            }
        }
    } else {
        ESP_LOGW(TAG, "wifi: no creds — entering AP onboarding");
    }

    if (ap_mode) {
        if (wifi_ap_up(AP_SETUP_SSID) != ESP_OK) {
            ESP_LOGE(TAG, "wifi: AP up failed");
            return ESP_FAIL;
        }
        if (http_ap_up() != ESP_OK) {
            ESP_LOGE(TAG, "http: AP server failed");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "ap: %s up — connect and visit http://192.168.4.1/",
                 AP_SETUP_SSID);
        /* HTTP server runs forever on its own task; api_setup reboots
         * after it persists the user's chosen creds to NVS. */
        return ESP_OK;
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
