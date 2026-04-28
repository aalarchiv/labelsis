#ifndef PT_SESSION_H
#define PT_SESSION_H

/*
 * pt_session — drive a complete print job over a pt_transport_t.
 *
 * Combines pt_protocol (byte encoding/decoding) with a pt_transport_t
 * (send/recv to a real or virtual printer). Transport-agnostic — works
 * with mock, libusb, or esp-idf usb_host backends.
 *
 * Flow per SDM §5.1 (concurrent over USB):
 *   invalidate → init → switch to raster mode → status check →
 *   print info → mode/advanced/margin → compression → raster rows →
 *   Ctrl-Z (print + feed) → poll for printing-completed status.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pt_protocol.h"
#include "pt_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pt_compression_t compression;        /* default TIFF (PackBits)      */
    bool             auto_cut;           /* default true                 */
    bool             no_chain_print;     /* default true (cut after job) */
    bool             mirror_print;       /* default false                */
    bool             special_tape;       /* default false                */
    uint16_t         margin_dots;        /* default 14 dots ≈ 2 mm @180  */
    bool             recover_always_on;  /* default true                 */
    uint32_t         status_timeout_ms;  /* per status-read, default 5000 */
    uint32_t         print_timeout_ms;   /* total job, default 60000     */
} pt_session_options_t;

/* Populate `out` with the documented defaults above. */
void pt_session_options_default(pt_session_options_t *out);

/* Read the printer's current status. Internally sends ESC i S and decodes
 * the 32-byte reply. */
pt_err_t pt_session_query_status(pt_transport_t *t,
                                 pt_status_t *out,
                                 const pt_session_options_t *opts);

/* Drive a complete single-page raster job.
 *
 *   t              : transport bound to the printer
 *   raster_rows    : pointer to n_rows * 16 bytes (one raster row each)
 *   n_rows         : number of raster rows (≤ ~7000 for a 1 m label at 180 dpi)
 *   media_width_mm : expected tape width; 0 = trust whatever is loaded
 *   opts           : NULL → defaults
 *
 * Returns PT_OK on confirmed successful print, or a pt_err_t describing
 * the printer-reported failure (NO_MEDIA, COVER_OPEN, etc.) or transport
 * failure. */
pt_err_t pt_session_print_raster(pt_transport_t *t,
                                 const uint8_t *raster_rows,
                                 size_t n_rows,
                                 uint8_t media_width_mm,
                                 const pt_session_options_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* PT_SESSION_H */
