/*
 * pt_session — orchestrates SDM §5.1 concurrent printing flow over an
 * abstract pt_transport_t. See pt_session.h.
 */

#include "pt_session.h"

#include <string.h>

/* ============================================================ helpers == */

void pt_session_options_default(pt_session_options_t *o)
{
    o->compression       = PT_COMPRESSION_TIFF;
    o->auto_cut          = true;
    o->no_chain_print    = true;
    o->mirror_print      = false;
    o->special_tape      = false;
    o->margin_dots       = 14;       /* ≈ 2 mm @ 180 dpi */
    o->recover_always_on = true;
    o->status_timeout_ms = 5000;
    o->print_timeout_ms  = 120000;  /* 2 min — tolerates a 1 m label + cooling */
    o->on_status         = NULL;
    o->on_status_user    = NULL;
    o->on_event          = NULL;
    o->on_event_user     = NULL;
}

/* Send the full buffer or fail. */
static pt_err_t send_all(pt_transport_t *t, const uint8_t *buf, size_t len)
{
    int sent = pt_transport_send(t, buf, len);
    if (sent < 0 || (size_t)sent != len) return PT_ERR_TRANSPORT;
    return PT_OK;
}

/* Read exactly 32 bytes of status into `out` within ~timeout_ms total.
 * Accumulates across multiple recv() calls so that a libusb short-read
 * (rare in practice — 32-byte status fits in one bulk packet — but
 * defensible) doesn't lose the partial bytes already pulled. */
static pt_err_t recv_status(pt_transport_t *t, pt_status_t *out,
                            uint32_t timeout_ms)
{
    uint8_t  resp[32];
    size_t   total = 0;
    uint32_t budget = timeout_ms;
    while (total < 32) {
        size_t got = 0;
        uint32_t step = (budget > 1000) ? 1000 : budget;
        int rc = pt_transport_recv(t, resp + total, 32 - total, &got, step);
        if (rc < 0) return PT_ERR_TRANSPORT;
        total += got;
        if (got == 0) {
            if (budget <= step) return PT_ERR_TIMEOUT;
            budget -= step;
        }
    }
    return pt_status_decode(resp, 32, out);
}

/* Map error1/error2 bit flags to a single pt_err_t. Highest-priority
 * error wins (no media first, then cutter jam, ...). Returns PT_OK if
 * no error bits are set. */
static pt_err_t map_errors(const pt_status_t *s)
{
    if (s->error1 & PT_ERR1_NO_MEDIA)      return PT_ERR_NO_MEDIA;
    if (s->error1 & PT_ERR1_CUTTER_JAM)    return PT_ERR_CUTTER_JAM;
    if (s->error1 & PT_ERR1_WEAK_BATTERY)  return PT_ERR_WEAK_BATTERY;
    if (s->error1 & PT_ERR1_HIGH_VOLTAGE)  return PT_ERR_HIGH_VOLTAGE;
    if (s->error2 & PT_ERR2_REPLACE_MEDIA) return PT_ERR_REPLACE_MEDIA;
    if (s->error2 & PT_ERR2_COVER_OPEN)    return PT_ERR_COVER_OPEN;
    if (s->error2 & PT_ERR2_OVERHEAT)      return PT_ERR_OVERHEAT;
    return PT_OK;
}

/* Send one raster row: 0x5A (zero-row shortcut), G+raw, or G+packbits
 * depending on row contents and selected compression. */
static pt_err_t send_row(pt_transport_t *t,
                         const uint8_t row[16],
                         pt_compression_t comp)
{
    static const uint8_t zero[16] = {0};
    uint8_t buf[64];
    int n;

    if (memcmp(row, zero, 16) == 0) {
        n = pt_encode_zero_row(buf, sizeof buf);
    } else if (comp == PT_COMPRESSION_TIFF) {
        uint8_t packed[40];
        int pn = pt_packbits_encode(packed, sizeof packed, row, 16);
        if (pn < 0) return PT_ERR_INVALID_ARG;
        n = pt_encode_raster_row(buf, sizeof buf, packed, (size_t)pn);
    } else {
        n = pt_encode_raster_row(buf, sizeof buf, row, 16);
    }
    if (n < 0) return PT_ERR_BUF_TOO_SMALL;
    return send_all(t, buf, (size_t)n);
}

/* PT-P700 completion semantics, observed empirically over libusb (Apr 2026):
 *
 *   - After Ctrl-Z the printer pushes ONE status message
 *     (status_type=phase-change, phase=printing) and then goes silent
 *     for the rest of the job — does NOT push the "printing-done" or
 *     trailing "phase-change → editing" that SDM §5.1 diagrams.
 *   - Does NOT respond to ESC i S sent after Ctrl-Z (verified: 24 polls
 *     over 2 min, zero replies).
 *   - Errors the printer DOES surface (cover-open / overheat / cutter-
 *     jam mid-print) come as unsolicited STATUS_ERROR_OCCURRED messages
 *     within the print window — we still want to catch them.
 *   - "No tape inside cartridge" is hardware-invisible — the cartridge-
 *     presence sensor doesn't detect tape-remaining. Print silently
 *     fails; user sees that visually.
 *
 * Strategy: estimate the physical print duration from the raster row
 * count, wait that long while draining any error messages the printer
 * might push, then return success. Without this wait pt_send would exit
 * after milliseconds (USB transfer time only), and a quick second
 * pt_send invocation would cancel the still-in-progress first print
 * via the SDM-documented ESC @ cancel-and-init. */
static pt_err_t wait_print_done(pt_transport_t *t,
                                size_t n_rows,
                                const pt_session_options_t *opts)
{
    /* PT-P700: ~30 mm/s tape speed = ~213 dots/s at 180 dpi, plus
     * head warm-up + feed-to-cutter + cut. Pad generously. */
    uint32_t est_ms = 1500u                        /* warm-up      */
                    + (uint32_t)(n_rows * 5u)      /* per-row time */
                    + 2000u;                       /* feed + cut   */
    if (est_ms > opts->print_timeout_ms) est_ms = opts->print_timeout_ms;

    uint32_t budget = est_ms;
    size_t   messages = 0;
    bool     seen_printing = false;

    while (budget > 0 && messages < 64) {
        uint32_t step = (budget > opts->status_timeout_ms)
                            ? opts->status_timeout_ms : budget;
        pt_status_t s;
        pt_err_t err = recv_status(t, &s, step);
        if (err == PT_ERR_TIMEOUT) {
            budget = (budget > step) ? budget - step : 0;
            continue;
        }
        if (err != PT_OK) return err;
        messages++;
        if (opts->on_status) opts->on_status(&s, opts->on_status_user);

        if (s.status_type == PT_STATUS_ERROR_OCCURRED) {
            pt_err_t e = map_errors(&s);
            return e ? e : PT_ERR_TRANSPORT;
        }
        /* Future-firmware-friendly: if the printer ever does push these,
         * we'll exit early. PT-P700 currently doesn't. */
        if (s.status_type == PT_STATUS_PRINTING_DONE) return PT_OK;
        if (s.phase_type == PT_PHASE_EDITING
            && (s.status_type == PT_STATUS_PHASE_CHANGE
             || s.status_type == PT_STATUS_REPLY)) return PT_OK;

        if (s.status_type == PT_STATUS_PHASE_CHANGE
            && s.phase_type == PT_PHASE_PRINTING) seen_printing = true;
    }
    if (opts->on_event)
        opts->on_event("estimated print time elapsed", opts->on_event_user);
    /* Saw phase=printing → printer accepted the job; declare success.
     * Never saw phase=printing → something's wrong upstream. */
    return seen_printing ? PT_OK : PT_ERR_TIMEOUT;
}

/* =============================================================== API == */

pt_err_t pt_session_query_status(pt_transport_t *t,
                                 pt_status_t *out,
                                 const pt_session_options_t *opts)
{
    if (!t || !out) return PT_ERR_INVALID_ARG;
    pt_session_options_t local;
    if (!opts) { pt_session_options_default(&local); opts = &local; }

    uint8_t cmd[8];
    int n = pt_encode_status_request(cmd, sizeof cmd);
    pt_err_t err = send_all(t, cmd, (size_t)n);
    if (err != PT_OK) return err;
    pt_err_t r = recv_status(t, out, opts->status_timeout_ms);
    if (r == PT_OK && opts->on_status) opts->on_status(out, opts->on_status_user);
    return r;
}

pt_err_t pt_session_print_raster(pt_transport_t *t,
                                 const uint8_t *raster_rows,
                                 size_t n_rows,
                                 uint8_t media_width_mm,
                                 const pt_session_options_t *opts)
{
    if (!t || !raster_rows || n_rows == 0) return PT_ERR_INVALID_ARG;
    pt_session_options_t local;
    if (!opts) { pt_session_options_default(&local); opts = &local; }

    uint8_t buf[128];
    int      n;
    pt_err_t err;

    /* 1. Invalidate + init. */
    n = pt_encode_invalidate(buf, sizeof buf);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;
    n = pt_encode_init(buf, sizeof buf);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;

    /* 2. Switch to raster mode. */
    n = pt_encode_switch_mode(buf, sizeof buf, PT_CMD_MODE_RASTER);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;

    /* 3. Status check — read media, surface any pre-existing errors. */
    pt_status_t st;
    if ((err = pt_session_query_status(t, &st, opts)) != PT_OK) return err;
    pt_err_t pre = map_errors(&st);
    if (pre != PT_OK) return pre;
    if (media_width_mm == 0) media_width_mm = st.media_width_mm;
    else if (st.media_width_mm != media_width_mm) return PT_ERR_MEDIA_MISMATCH;

    /* 4. Print info. */
    pt_print_info_t info = {
        .media_type        = st.media_type,
        .media_width_mm    = media_width_mm,
        .media_length_mm   = 0,
        .raster_lines      = (uint32_t)n_rows,
        .is_first_page     = true,
        .valid_kind        = true,
        .valid_width       = true,
        .recover_always_on = opts->recover_always_on,
    };
    n = pt_encode_print_info(buf, sizeof buf, &info);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;

    /* 5. Mode + advanced settings. */
    pt_mode_settings_t m = {
        .auto_cut = opts->auto_cut, .mirror_print = opts->mirror_print
    };
    n = pt_encode_mode_settings(buf, sizeof buf, &m);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;

    pt_advanced_settings_t a = {
        .no_chain_print = opts->no_chain_print, .special_tape = opts->special_tape
    };
    n = pt_encode_advanced_settings(buf, sizeof buf, &a);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;

    /* 6. Margin + compression. */
    n = pt_encode_margin(buf, sizeof buf, opts->margin_dots);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;
    n = pt_encode_compression(buf, sizeof buf, opts->compression);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;

    /* 7. Raster rows. */
    for (size_t r = 0; r < n_rows; r++) {
        if ((err = send_row(t, raster_rows + r * 16, opts->compression)) != PT_OK)
            return err;
    }

    /* 8. Print + feed. */
    n = pt_encode_print_last(buf, sizeof buf);
    if ((err = send_all(t, buf, (size_t)n)) != PT_OK) return err;

    /* 9. Wait for printing-completed. */
    return wait_print_done(t, n_rows, opts);
}
