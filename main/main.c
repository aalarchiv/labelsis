#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pt_protocol.h"
#include "pt_transport_mock.h"

static const char *TAG = "pt700";

/* On-target smoke test: drive a short job through the mock transport and
 * log its outcome. Proves pt_protocol + pt_transport_mock are alive on
 * ESP32-S3. Real USB host transport + HTTP app land in later phases. */
void app_main(void)
{
    ESP_LOGI(TAG, "pt700 print server booting");

    static pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    uint8_t cmd[8];
    int n = pt_encode_status_request(cmd, sizeof cmd);
    pt_transport_send(&t, cmd, (size_t)n);

    uint8_t resp[64];
    size_t  resp_len = 0;
    pt_transport_recv(&t, resp, sizeof resp, &resp_len, 100);

    pt_status_t status;
    if (pt_status_decode(resp, resp_len, &status) == PT_OK) {
        ESP_LOGI(TAG, "mock printer: model=0x%02x media=%umm type=0x%02x",
                 status.model, status.media_width_mm, status.media_type);
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
