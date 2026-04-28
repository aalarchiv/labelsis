/*
 * pt_protocol — encoder and decoder for the Brother PT-H500/P700/E500
 * raster command set. Pure C, no I/O.
 *
 * All byte layouts come from refs/brother/cv_pth500p700e500_eng_raster_111.pdf
 * (SDM v1.11). Section/page citations appear inline.
 */

#include "pt_protocol.h"

#include <string.h>

/* ========================================================== tape geom == */

/* SDM p. 20 TZe table — left margin pins | print area pins | right pins
 * for the H500/P700/E500 (128-pin head). The printer reports the tape
 * width via the status byte 10 as 4 / 6 / 9 / 12 / 18 / 24 (rounded up
 * for the 3.5 mm tape — SDM table (3), p. 27). */
pt_err_t pt_tape_geometry_tze(uint8_t width_mm, pt_tape_geometry_t *out)
{
    if (!out) return PT_ERR_INVALID_ARG;
    static const struct { uint8_t w, left, print, right; } table[] = {
        {  4, 52,  24, 52 },  /* 3.5 mm tape — status byte reports 4 */
        {  6, 48,  32, 48 },
        {  9, 39,  50, 39 },
        { 12, 29,  70, 29 },
        { 18,  8, 112,  8 },
        { 24,  0, 128,  0 },
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (table[i].w == width_mm) {
            out->total_pins         = 128;
            out->left_margin_pins   = table[i].left;
            out->print_pins         = table[i].print;
            out->right_margin_pins  = table[i].right;
            return PT_OK;
        }
    }
    return PT_ERR_INVALID_ARG;
}

/* ============================================================ status === */

pt_err_t pt_status_decode(const uint8_t *buf, size_t len, pt_status_t *out)
{
    if (!buf || !out) return PT_ERR_INVALID_ARG;
    if (len != 32)    return PT_ERR_BAD_LEN;
    if (buf[0] != 0x80) return PT_ERR_BAD_MAGIC;

    out->model             = (pt_model_t)buf[4];
    out->error1            = buf[8];
    out->error2            = buf[9];
    out->media_width_mm    = buf[10];
    out->media_type        = (pt_media_type_t)buf[11];
    out->mode              = buf[15];
    out->status_type       = (pt_status_type_t)buf[18];
    out->phase_type        = (pt_phase_type_t)buf[19];
    out->phase_number      = (uint16_t)((buf[20] << 8) | buf[21]);
    out->notification      = (pt_notification_t)buf[22];
    out->tape_color_id     = buf[24];
    out->text_color_id     = buf[25];
    out->hardware_settings = ((uint32_t)buf[26] << 24)
                           | ((uint32_t)buf[27] << 16)
                           | ((uint32_t)buf[28] <<  8)
                           |  (uint32_t)buf[29];
    return PT_OK;
}

/* =========================================================== encoders == */

int pt_encode_invalidate(uint8_t *buf, size_t cap)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 100) return PT_ERR_BUF_TOO_SMALL;
    memset(buf, 0x00, 100);
    return 100;
}

int pt_encode_init(uint8_t *buf, size_t cap)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 2) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x1b; buf[1] = 0x40;
    return 2;
}

int pt_encode_status_request(uint8_t *buf, size_t cap)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 3) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x1b; buf[1] = 0x69; buf[2] = 0x53;
    return 3;
}

int pt_encode_switch_mode(uint8_t *buf, size_t cap, pt_command_mode_t mode)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 4) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x1b; buf[1] = 0x69; buf[2] = 0x61;
    buf[3] = (uint8_t)mode;
    return 4;
}

int pt_encode_print_info(uint8_t *buf, size_t cap, const pt_print_info_t *info)
{
    if (!buf || !info) return PT_ERR_INVALID_ARG;
    if (cap < 13) return PT_ERR_BUF_TOO_SMALL;

    /* {n1} valid-flags bitfield (SDM §4 ESC i z, p. 32). */
    uint8_t valid = 0;
    if (info->valid_kind)        valid |= 0x02;  /* PI_KIND    */
    if (info->valid_width)       valid |= 0x04;  /* PI_WIDTH   */
    if (info->valid_length)      valid |= 0x08;  /* PI_LENGTH  */
    if (info->priority_quality)  valid |= 0x40;  /* PI_QUALITY */
    if (info->recover_always_on) valid |= 0x80;  /* PI_RECOVER */

    buf[0] = 0x1b; buf[1] = 0x69; buf[2] = 0x7a;
    buf[3]  = valid;
    buf[4]  = (uint8_t)info->media_type;
    buf[5]  = info->media_width_mm;
    buf[6]  = info->media_length_mm;
    /* {n5..n8}: raster line count, little-endian. */
    buf[7]  = (uint8_t)( info->raster_lines        & 0xff);
    buf[8]  = (uint8_t)((info->raster_lines >>  8) & 0xff);
    buf[9]  = (uint8_t)((info->raster_lines >> 16) & 0xff);
    buf[10] = (uint8_t)((info->raster_lines >> 24) & 0xff);
    /* {n9}: starting page = 0; subsequent pages = 1. */
    buf[11] = info->is_first_page ? 0x00 : 0x01;
    buf[12] = 0x00;  /* {n10} fixed at 0 */
    return 13;
}

int pt_encode_mode_settings(uint8_t *buf, size_t cap, const pt_mode_settings_t *m)
{
    if (!buf || !m) return PT_ERR_INVALID_ARG;
    if (cap < 4) return PT_ERR_BUF_TOO_SMALL;
    uint8_t bits = 0;
    if (m->auto_cut)     bits |= 0x40;  /* bit 6 */
    if (m->mirror_print) bits |= 0x80;  /* bit 7 */
    buf[0] = 0x1b; buf[1] = 0x69; buf[2] = 0x4d;
    buf[3] = bits;
    return 4;
}

int pt_encode_advanced_settings(uint8_t *buf, size_t cap,
                                const pt_advanced_settings_t *a)
{
    if (!buf || !a) return PT_ERR_INVALID_ARG;
    if (cap < 4) return PT_ERR_BUF_TOO_SMALL;
    uint8_t bits = 0;
    if (a->no_chain_print)  bits |= 0x08;  /* bit 3 */
    if (a->special_tape)    bits |= 0x10;  /* bit 4 */
    if (a->no_buffer_clear) bits |= 0x80;  /* bit 7 */
    buf[0] = 0x1b; buf[1] = 0x69; buf[2] = 0x4b;
    buf[3] = bits;
    return 4;
}

int pt_encode_margin(uint8_t *buf, size_t cap, uint16_t margin_dots)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 5) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x1b; buf[1] = 0x69; buf[2] = 0x64;
    buf[3] = (uint8_t)( margin_dots       & 0xff);
    buf[4] = (uint8_t)((margin_dots >> 8) & 0xff);
    return 5;
}

int pt_encode_compression(uint8_t *buf, size_t cap, pt_compression_t mode)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 2) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x4d;
    buf[1] = (uint8_t)mode;
    return 2;
}

int pt_encode_raster_row(uint8_t *buf, size_t cap,
                         const uint8_t *row, size_t row_len)
{
    if (!buf || !row) return PT_ERR_INVALID_ARG;
    if (row_len > 0xffff) return PT_ERR_INVALID_ARG;
    if (cap < 3 + row_len) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x67;  /* 'g' */
    buf[1] = (uint8_t)( row_len       & 0xff);
    buf[2] = (uint8_t)((row_len >> 8) & 0xff);
    memcpy(buf + 3, row, row_len);
    return (int)(3 + row_len);
}

int pt_encode_zero_row(uint8_t *buf, size_t cap)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 1) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x5a;
    return 1;
}

int pt_encode_print_page(uint8_t *buf, size_t cap)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 1) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x0c;  /* FF */
    return 1;
}

int pt_encode_print_last(uint8_t *buf, size_t cap)
{
    if (!buf) return PT_ERR_INVALID_ARG;
    if (cap < 1) return PT_ERR_BUF_TOO_SMALL;
    buf[0] = 0x1a;  /* Ctrl-Z */
    return 1;
}

/* =========================================================== packbits == */

/* PackBits encoder per SDM §4 M (p. 35). Strict-SDM: a run of 2 equal
 * bytes encodes as a repeat code (matching the SDM example). Raw chunks
 * extend until either input ends or the next byte equals its successor
 * (i.e. a new repeat starts). Max chunk size 128 (signed-byte count limit).
 *
 * If the compressed stream for a 16-byte input exceeds 16 bytes, falls
 * back to the SDM-documented raw 17-byte form: 0x0F + 16 raw bytes.
 *
 * NB: refs/ptouch-770 makes different (also-spec-valid) choices — see
 * bd memory "pt700 PACKBITS DIVERGENCE". */
int pt_packbits_encode(uint8_t *out, size_t cap,
                       const uint8_t *in, size_t in_len)
{
    if (!out || !in) return PT_ERR_INVALID_ARG;
    if (in_len == 0) return 0;

    /* Stage to a scratch buffer first so we can apply the >16-byte
     * fallback rule before writing to the caller's output. */
    uint8_t tmp[256];
    if (in_len * 2 + 2 > sizeof tmp) return PT_ERR_BUF_TOO_SMALL;

    size_t out_len = 0;
    size_t i = 0;
    while (i < in_len) {
        /* Detect a repeat run starting at i, capped at 128 bytes. */
        size_t j = i + 1;
        while (j < in_len && in[j] == in[i] && (j - i) < 128) j++;
        size_t run = j - i;

        if (run >= 2) {
            tmp[out_len++] = (uint8_t)(int8_t)(1 - (int)run);
            tmp[out_len++] = in[i];
            i = j;
        } else {
            /* Raw chunk: keep going while no new repeat starts at the
             * lookahead position (and we haven't hit the 128-byte cap). */
            size_t k = i + 1;
            while (k < in_len
                   && (k + 1 >= in_len || in[k] != in[k + 1])
                   && (k - i) < 128) k++;
            size_t raw = k - i;
            tmp[out_len++] = (uint8_t)(raw - 1);
            for (size_t m = 0; m < raw; m++) tmp[out_len++] = in[i + m];
            i = k;
        }
    }

    /* SDM fallback: if compressed > 16 bytes for a 16-byte row, emit
     * raw 17-byte form (length=15 + 16 bytes). */
    if (in_len == 16 && out_len > 16) {
        if (cap < 17) return PT_ERR_BUF_TOO_SMALL;
        out[0] = 0x0f;
        memcpy(out + 1, in, 16);
        return 17;
    }

    if (cap < out_len) return PT_ERR_BUF_TOO_SMALL;
    memcpy(out, tmp, out_len);
    return (int)out_len;
}

int pt_packbits_decode(uint8_t *out, size_t cap,
                       const uint8_t *in, size_t in_len)
{
    if (!out || !in) return PT_ERR_INVALID_ARG;
    size_t i = 0, o = 0;
    while (i < in_len) {
        int8_t code = (int8_t)in[i++];
        if (code >= 0) {
            size_t n = (size_t)code + 1;
            if (i + n > in_len) return PT_ERR_INVALID_ARG;
            if (o + n > cap)    return PT_ERR_BUF_TOO_SMALL;
            memcpy(out + o, in + i, n);
            i += n; o += n;
        } else {
            size_t n = (size_t)(1 - code);
            if (i >= in_len) return PT_ERR_INVALID_ARG;
            if (o + n > cap) return PT_ERR_BUF_TOO_SMALL;
            memset(out + o, in[i], n);
            i++; o += n;
        }
    }
    return (int)o;
}

/* ============================================================ bitmap === */

/* Layout convention (matches the .pbm fixtures in refs/ptouch-770):
 *   - bitmap is row-major, MSB-first within byte.
 *   - "row" runs along the cross-tape direction (= pin direction).
 *   - "col" runs along the feed direction; one feed step per output row.
 *
 * pixels_across must equal geom->print_pins. Each output raster row is
 * 16 bytes; the function shifts pixel column 0 to start at print head pin
 * `geom->left_margin_pins`, leaving margin pins zero. */
pt_err_t pt_bitmap_to_raster(
    const uint8_t            *bitmap,
    size_t                    pixels_across,
    size_t                    raster_lines,
    const pt_tape_geometry_t *geom,
    uint8_t                  *out,
    size_t                   *out_rows)
{
    if (!bitmap || !geom || !out || !out_rows) return PT_ERR_INVALID_ARG;
    if (pixels_across != geom->print_pins) return PT_ERR_INVALID_ARG;
    if ((size_t)geom->left_margin_pins + pixels_across > geom->total_pins)
        return PT_ERR_INVALID_ARG;

    size_t row_stride = (raster_lines + 7) / 8;

    for (size_t line = 0; line < raster_lines; line++) {
        uint8_t *row_out = out + line * 16;
        memset(row_out, 0, 16);
        for (size_t pix = 0; pix < pixels_across; pix++) {
            /* Source bit: (pix-th bitmap row, line-th column). */
            uint8_t src_byte = bitmap[pix * row_stride + line / 8];
            if (src_byte & (uint8_t)(0x80 >> (line % 8))) {
                size_t pin = (size_t)geom->left_margin_pins + pix;
                row_out[pin / 8] |= (uint8_t)(0x80 >> (pin % 8));
            }
        }
    }
    *out_rows = raster_lines;
    return PT_OK;
}

/* Placeholder kept for the on-target main.c smoke test until Phase 4
 * wires in pt_session. Will be removed when no longer referenced. */
void pt_protocol_placeholder(void) {}
