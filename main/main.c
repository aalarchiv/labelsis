#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pt_protocol.h"
#include "pt_transport.h"

static const char *TAG = "pt700";

void app_main(void)
{
    ESP_LOGI(TAG, "pt700 print server skeleton booting");
    pt_protocol_placeholder();
    pt_transport_mock_placeholder();

    while (1) {
        ESP_LOGI(TAG, "tick");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
