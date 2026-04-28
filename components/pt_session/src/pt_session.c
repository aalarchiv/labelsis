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

/* After Ctrl-Z, the printer emits Phase=Printing, Printing-done, then
 * Phase=Editing per SDM §5.1. Loop reading status messages until we see
 * PRINTING_DONE or an error. Bound the loop two ways: a print-time
 * budget that ticks down only on TIMEOUT (so a chatty sequence of phase
 * changes doesn't burn the budget), and a hard cap of 64 messages as a
 * runaway guard. */
static pt_err_t wait_print_done(pt_transport_t *t,
                                const pt_session_options_t *opts)
{
    const uint32_t step = opts->status_timeout_ms;
    uint32_t       budget   = opts->print_timeout_ms;
    size_t         messages = 0;
    while (budget > 0 && messages < 64) {
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
        if (s.status_type == PT_STATUS_PRINTING_DONE) return PT_OK;
        /* Real hardware sometimes skips PRINTING_DONE and jumps straight
         * to PHASE_CHANGE → editing once the page is finished — accept
         * that as completion too. (SDM §5.1 documents both messages but
         * observed PT-P700 behaviour omits PRINTING_DONE in some configs.) */
        if (s.status_type == PT_STATUS_PHASE_CHANGE
            && s.phase_type == PT_PHASE_EDITING)
            return PT_OK;
        /* notifications, phase=printing, etc. — keep reading without burning budget */
    }
    return PT_ERR_TIMEOUT;
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
    return wait_print_done(t, opts);
}
