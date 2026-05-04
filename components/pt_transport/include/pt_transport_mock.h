#ifndef PT_TRANSPORT_MOCK_H
#define PT_TRANSPORT_MOCK_H

/*
 * Virtual PT-P700 backing a pt_transport_t. Used by host tests and by
 * any on-target test that wants to drive pt_session without a real
 * printer attached.
 *
 * The state machine mirrors SDM §5 flow charts at a coarse level:
 *   - on ESC i S         → enqueue a 32-byte status reply
 *   - on Print / +Feed   → enqueue Phase=Printing, Printing-done,
 *                          Phase=Editing transitions per §5.1
 *   - any error1/error2  → status_type forced to ERROR_OCCURRED
 *
 * Bytes received from the host are appended to a captured TX buffer the
 * caller can inspect, and parsed incrementally so split sends work.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pt_protocol.h"
#include "pt_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sized buffers so the mock can live in static storage with no malloc. */
#define PT_MOCK_TX_CAP 4096
#define PT_MOCK_RX_CAP  512

typedef struct {
    /* Virtual cartridge & printer state. */
    pt_model_t        model;
    uint8_t           media_width_mm;
    pt_media_type_t   media_type;
    uint8_t           tape_color_id;
    uint8_t           text_color_id;
    uint8_t           error1;          /* PT_ERR1_* flags */
    uint8_t           error2;          /* PT_ERR2_* flags */
    pt_notification_t notification;

    /* Job-time observable counters. */
    pt_command_mode_t cmd_mode;
    pt_compression_t  compression;
    uint32_t          rows_received;
    uint32_t          pages_completed;

    /* Captured TX (host → printer) for caller inspection + incremental
     * parsing cursor. */
    uint8_t  tx_buf[PT_MOCK_TX_CAP];
    size_t   tx_len;
    size_t   tx_parsed;

    /* Pending RX (printer → host) ring buffer. */
    uint8_t  rx_buf[PT_MOCK_RX_CAP];
    size_t   rx_head;
    size_t   rx_tail;
    size_t   rx_len;
} pt_transport_mock_t;

/* Initialise mock with sensible defaults: PT-P700, 24 mm laminated white
 * tape with black text, no errors, idle. Returns a pt_transport_t bound
 * to *m so callers can pass &transport into pt_session. */
pt_transport_t pt_transport_mock_init(pt_transport_mock_t *m);

/* Reset internal counters, parser cursor, and rx/tx queues. Cartridge
 * configuration (media, errors) is preserved. */
void pt_transport_mock_reset(pt_transport_mock_t *m);

/* Configure the virtual cartridge - has effect on the next status reply. */
void pt_transport_mock_set_media(pt_transport_mock_t *m,
                                 uint8_t width_mm,
                                 pt_media_type_t type,
                                 uint8_t tape_color_id,
                                 uint8_t text_color_id);

/* Set / clear error flag bits (PT_ERR1_* / PT_ERR2_*). They persist
 * until overwritten with 0. */
void pt_transport_mock_set_error1(pt_transport_mock_t *m, uint8_t flags);
void pt_transport_mock_set_error2(pt_transport_mock_t *m, uint8_t flags);

/* Inspect captured TX bytes (everything send() has accepted). */
size_t          pt_transport_mock_tx_size(const pt_transport_mock_t *m);
const uint8_t  *pt_transport_mock_tx_data(const pt_transport_mock_t *m);

#ifdef __cplusplus
}
#endif

#endif /* PT_TRANSPORT_MOCK_H */
