#ifndef PT_APP_H
#define PT_APP_H

/*
 * pt_app — MVP HTTP print-server glue for the PT-* on ESP32-S2/S3.
 * Brings up Wi-Fi STA, opens a transport (real USB host or mock),
 * starts an esp_http_server, and registers the JSON API endpoints
 * pt_session is exposed through.
 *
 * Single-call entry point from main: pt_app_run(&cfg). Phase-5 follow-up
 * issues (AP-mode onboarding, /api/print, SPA, mDNS, /api/feed,
 * /api/cut) extend this without changing the call shape.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Optional first-boot fallback creds. If NVS has stored creds, those
     * win; otherwise these are tried, and persisted to NVS on first
     * successful STA connect. NULL or empty → skip straight to AP-mode
     * onboarding. */
    const char *wifi_ssid;
    const char *wifi_password;
    uint16_t    http_port;       /* default 80 if 0 */
    bool        use_usb_host;    /* false → mock transport (no printer needed) */
    uint32_t    usb_connect_timeout_ms;  /* default 10000 if 0 */
    /* Active-low button (with internal pull-up) that, when held for
     * 5 s, wipes Wi-Fi creds from NVS and reboots into AP-mode
     * onboarding. Set to a negative value to disable. GPIO 0 is the
     * BOOT button on most ESP32-S2/S3 devkits. */
    int         reset_gpio_num;
} pt_app_config_t;

/* Bring everything up. Returns ESP_OK once the HTTP server is listening
 * (Wi-Fi connected, transport opened, routes registered). Blocks until
 * Wi-Fi is up or fails after the internal retry budget. After ESP_OK
 * the app keeps running on the HTTP server's tasks; main can return. */
esp_err_t pt_app_run(const pt_app_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* PT_APP_H */
