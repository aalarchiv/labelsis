#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pt_app.h"

/* Per-board pin/LED defaults. boards/<name>/board.h defines BOARD_NAME,
 * BOARD_LED_{TYPE,GPIO,ACTIVE_LOW}, BOARD_RESET_GPIO. The top-level
 * CMakeLists.txt picks <name> from -D BOARD=... and adds boards/<name>/
 * to the include path; see boards/README.md for adding a new one. */
#include "board.h"

/* Real Wi-Fi creds, gitignored -- copy main/wifi_credentials.example.h
 * to main/wifi_credentials.h and fill in your SSID + password. Falls
 * back to placeholder values when absent so CI / sandbox builds still
 * succeed (they just won't associate). */
#if __has_include("wifi_credentials.h")
#  include "wifi_credentials.h"
#else
#  define WIFI_SSID     "labelsis-build-no-creds"
#  define WIFI_PASSWORD "set-via-wifi_credentials.h"
#endif

static const char *TAG = "labelsis";

void app_main(void)
{
    ESP_LOGI(TAG, "LabelSis print server booting on %s", BOARD_NAME);

    pt_app_config_t cfg = {
        .wifi_ssid              = WIFI_SSID,
        .wifi_password          = WIFI_PASSWORD,
        .http_port              = 80,
        .use_usb_host           = true,   /* try the real printer first;
                                             falls back to mock after the
                                             usb_connect_timeout below */
        .usb_connect_timeout_ms = 5000,
        .reset_gpio_num         = BOARD_RESET_GPIO,
        /* Status LED -- board profile picks the type/pin. To disable
         * entirely on a board that has no usable LED, set
         *   #define BOARD_LED_TYPE PT_LED_TYPE_NONE
         * in boards/<name>/board.h. Cadence/colour table lives in
         * components/pt_app/src/pt_led.c. */
        .led = {
            .type       = BOARD_LED_TYPE,
            .gpio       = BOARD_LED_GPIO,
            .active_low = BOARD_LED_ACTIVE_LOW,
        },
    };
    pt_app_run(&cfg);

    /* HTTP server runs on its own task; main can sleep. */
    while (1) vTaskDelay(pdMS_TO_TICKS(60000));
}
