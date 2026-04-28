#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pt_app.h"

/* Real Wi-Fi creds, gitignored — copy main/wifi_credentials.example.h
 * to main/wifi_credentials.h and fill in your SSID + password. Falls
 * back to placeholder values when absent so CI / sandbox builds still
 * succeed (they just won't associate). */
#if __has_include("wifi_credentials.h")
#  include "wifi_credentials.h"
#else
#  define WIFI_SSID     "pt700-build-no-creds"
#  define WIFI_PASSWORD "set-via-wifi_credentials.h"
#endif

static const char *TAG = "pt700";

void app_main(void)
{
    ESP_LOGI(TAG, "pt700 print server booting");

    pt_app_config_t cfg = {
        .wifi_ssid              = WIFI_SSID,
        .wifi_password          = WIFI_PASSWORD,
        .http_port              = 80,
        .use_usb_host           = true,   /* try the real printer first;
                                             falls back to mock after the
                                             usb_connect_timeout below */
        /* Generous timeout: a printer in P-Lite mode at cold boot
         * triggers our MSC unstick + re-enumeration cycle, which has
         * been observed to take ~25 s before the printer comes back
         * as PID 0x2061. The wait short-circuits as soon as a real
         * printer pairs, so the only cost of a long cap is the
         * fall-through-to-mock path when no printer is attached. */
        .usb_connect_timeout_ms = 30000,
        .reset_gpio_num         = 0,      /* BOOT button on most devkits;
                                             5 s hold wipes Wi-Fi creds */
    };
    pt_app_run(&cfg);

    /* HTTP server runs on its own task; main can sleep. */
    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
