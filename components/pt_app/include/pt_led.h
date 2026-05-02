#ifndef PT_LED_H
#define PT_LED_H

/*
 * pt_led -- single status LED driver.
 *
 * Two backends: a plain GPIO (one colour, distinguished by blink
 * cadence) or a single WS2812 RGB pixel (distinguished by colour and
 * cadence). Picked at boot via pt_led_config_t.
 *
 * pt_led_set() is callable from any task, ISR-unsafe. The actual LED
 * is driven by an internal task that polls the requested state at
 * 50 Hz, so callers never block on hardware.
 */

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PT_LED_TYPE_NONE = 0, /* no LED wired -- pt_led_set() is a no-op */
    PT_LED_TYPE_GPIO,     /* single LED on one GPIO (active-high or low) */
    PT_LED_TYPE_RGB,      /* WS2812 single-pixel on one GPIO */
} pt_led_type_t;

typedef struct {
    pt_led_type_t type;
    int           gpio;       /* unused when type == PT_LED_TYPE_NONE */
    bool          active_low; /* PT_LED_TYPE_GPIO only */
} pt_led_config_t;

/* Stages the LED reflects. The cadence/colour table lives in pt_led.c
 * so all visible UX tweaks happen in one place. */
typedef enum {
    PT_LED_BOOT,            /* default at startup */
    PT_LED_STA_CONNECTING,
    PT_LED_AP_ONBOARDING,
    PT_LED_READY,           /* Wi-Fi up + printer attached */
    PT_LED_USB_WAITING,     /* Wi-Fi up but no PT-* on USB */
    PT_LED_PRINTING,        /* transient, restored by caller after job */
    PT_LED_ERROR,           /* unrecoverable -- not auto-set today */
} pt_led_state_t;

/* Spawn the driver task with the given backend. Returns ESP_OK on
 * success or when type==NONE (which is a deliberate no-op). Safe to
 * call exactly once per boot. */
esp_err_t pt_led_init(const pt_led_config_t *cfg);

/* Switch the displayed state. Cheap and lock-free. No-op when init
 * was never called or type is NONE. */
void pt_led_set(pt_led_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* PT_LED_H */
