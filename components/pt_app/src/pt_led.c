#include "pt_led.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "pt_led";

/* Cadence table. period_ms = full cycle, on_ms = part of the cycle the
 * LED is lit. r/g/b apply only to the WS2812 backend; the plain-GPIO
 * backend uses cadence alone. Picked so each state is distinguishable
 * on a single LED:
 *
 *   BOOT             very fast strobe (transient, before any creds)
 *   STA_CONNECTING   single short blip every second
 *   AP_ONBOARDING    fast steady blink (3-4 Hz)
 *   READY            solid
 *   USB_WAITING      lonely blip every 2 s ("here, but no printer")
 *   PRINTING         very fast strobe (clearly active)
 *   ERROR            even 1 Hz blink (50% duty)
 */
typedef struct {
    uint16_t period_ms;
    uint16_t on_ms;
    uint8_t  r, g, b;
} led_pattern_t;

static const led_pattern_t PATTERNS[] = {
    [PT_LED_BOOT]            = { 200,  100,  255, 200, 0   }, /* yellow */
    [PT_LED_STA_CONNECTING]  = { 1000, 100,  0,   0,   255 }, /* blue */
    [PT_LED_AP_ONBOARDING]   = { 300,  150,  255, 0,   255 }, /* magenta */
    [PT_LED_READY]           = { 1000, 1000, 0,   255, 0   }, /* green, solid */
    [PT_LED_USB_WAITING]     = { 2000, 100,  255, 96,  0   }, /* orange */
    [PT_LED_PRINTING]        = { 100,  50,   255, 255, 255 }, /* white strobe */
    [PT_LED_ERROR]           = { 1000, 500,  255, 0,   0   }, /* red, 1 Hz */
};

static volatile pt_led_state_t s_state = PT_LED_BOOT;
static pt_led_config_t         s_cfg;
static led_strip_handle_t      s_strip;

#define LED_TICK_MS 20

static void drive_gpio(bool on)
{
    int level = on ? 1 : 0;
    if (s_cfg.active_low) level = !level;
    gpio_set_level(s_cfg.gpio, level);
}

static void drive_rgb(bool on, const led_pattern_t *p)
{
    led_strip_set_pixel(s_strip, 0,
                        on ? p->r : 0,
                        on ? p->g : 0,
                        on ? p->b : 0);
    led_strip_refresh(s_strip);
}

static void led_task(void *arg)
{
    pt_led_state_t cur = s_state;
    TickType_t     phase_start = xTaskGetTickCount();

    for (;;) {
        if (s_state != cur) {
            cur = s_state;
            phase_start = xTaskGetTickCount();
        }
        const led_pattern_t *p = &PATTERNS[cur];
        uint32_t elapsed_ms = (xTaskGetTickCount() - phase_start) *
                              portTICK_PERIOD_MS;
        uint32_t phase = elapsed_ms % p->period_ms;
        bool     on    = (phase < p->on_ms);

        if (s_cfg.type == PT_LED_TYPE_GPIO) drive_gpio(on);
        else                                drive_rgb(on, p);

        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

esp_err_t pt_led_init(const pt_led_config_t *cfg)
{
    if (!cfg || cfg->type == PT_LED_TYPE_NONE) {
        ESP_LOGI(TAG, "no LED configured");
        return ESP_OK;
    }
    s_cfg = *cfg;

    if (cfg->type == PT_LED_TYPE_GPIO) {
        gpio_config_t gc = {
            .pin_bit_mask = 1ULL << cfg->gpio,
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&gc) != ESP_OK) {
            ESP_LOGE(TAG, "gpio %d config failed", cfg->gpio);
            s_cfg.type = PT_LED_TYPE_NONE;
            return ESP_FAIL;
        }
        drive_gpio(false);
        ESP_LOGI(TAG, "gpio %d, active-%s",
                 cfg->gpio, cfg->active_low ? "low" : "high");
    } else if (cfg->type == PT_LED_TYPE_RGB) {
        led_strip_config_t scfg = {
            .strip_gpio_num         = cfg->gpio,
            .max_leds               = 1,
            .led_model              = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        };
        led_strip_rmt_config_t rmt = {
            .clk_src       = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000,
            .flags.with_dma = false,
        };
        if (led_strip_new_rmt_device(&scfg, &rmt, &s_strip) != ESP_OK) {
            ESP_LOGE(TAG, "ws2812 init failed on gpio %d", cfg->gpio);
            s_cfg.type = PT_LED_TYPE_NONE;
            return ESP_FAIL;
        }
        led_strip_clear(s_strip);
        ESP_LOGI(TAG, "ws2812 on gpio %d", cfg->gpio);
    }

    BaseType_t r = xTaskCreate(led_task, "pt_led", 2048, NULL, 1, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed");
        s_cfg.type = PT_LED_TYPE_NONE;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void pt_led_set(pt_led_state_t state)
{
    s_state = state;
}
