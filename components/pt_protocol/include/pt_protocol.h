#ifndef PT_PROTOCOL_H
#define PT_PROTOCOL_H

/*
 * pt_protocol — Brother PT-H500/P700/E500 raster command encoder & decoder.
 *
 * Pure C11. No I/O, no esp-idf, no allocations. Encodes command bytes into
 * caller-provided buffers and decodes byte streams the printer returns.
 * Transport (USB host or mock) is a separate layer — see pt_transport.h.
 *
 * All formats come from "PT-H500/P700/E500 Raster Command Reference" v1.11,
 *   refs/brother/cv_pth500p700e500_eng_raster_111.pdf.
 * Section/table citations appear inline. Page numbers are PDF-page,
 * not section numbers, where ambiguous.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================== errors == */

typedef enum {
    PT_OK                =  0,
    PT_ERR_BUF_TOO_SMALL = -1,  /* output buffer cannot hold the result */
    PT_ERR_INVALID_ARG   = -2,  /* unknown enum / unsupported tape / etc. */
    PT_ERR_BAD_LEN       = -3,  /* status buffer length != 32             */
    PT_ERR_BAD_MAGIC     = -4,  /* status[0] (print-head mark) != 0x80    */
} pt_err_t;

/* =============================================================== model == */

/* Status byte 4 — printer model code (SDM §4 status table, p. 24). */
typedef enum {
    PT_MODEL_H500 = 0x64,  /* 'd' */
    PT_MODEL_E500 = 0x65,  /* 'e' */
    PT_MODEL_P700 = 0x67,  /* 'g' */
} pt_model_t;

/* =============================================================== media == */

/* Status byte 11 / ESC i z {n2} — media type (SDM §4 table (4), p. 27). */
typedef enum {
    PT_MEDIA_NONE           = 0x00,
    PT_MEDIA_LAMINATED      = 0x01,
    PT_MEDIA_NON_LAMINATED  = 0x03,
    PT_MEDIA_HEATSHRINK_2_1 = 0x11,
    PT_MEDIA_HEATSHRINK_3_1 = 0x17,
    PT_MEDIA_INCOMPATIBLE   = 0xFF,
} pt_media_type_t;

/* Print head pin geometry for a given tape. Total is always 128 on the
 * H500/P700/E500. left + print + right == total. (SDM p. 20 TZe table.) */
typedef struct {
    uint8_t total_pins;
    uint8_t print_pins;
    uint8_t left_margin_pins;
    uint8_t right_margin_pins;
} pt_tape_geometry_t;

/* Look up pin geometry for a TZe tape width in mm.
 * Supported widths: 3 (a.k.a. 3.5), 6, 9, 12, 18, 24. */
pt_err_t pt_tape_geometry_tze(uint8_t width_mm, pt_tape_geometry_t *out);

/* ====================================================== error bit flags == */

/* Status byte 8 — "error information 1" (SDM §4 table (1), p. 26). */
#define PT_ERR1_NO_MEDIA      0x01
#define PT_ERR1_CUTTER_JAM    0x04
#define PT_ERR1_WEAK_BATTERY  0x08
#define PT_ERR1_HIGH_VOLTAGE  0x40

/* Status byte 9 — "error information 2" (SDM §4 table (2), p. 26). */
#define PT_ERR2_REPLACE_MEDIA 0x01
#define PT_ERR2_COVER_OPEN    0x10
#define PT_ERR2_OVERHEAT      0x20

/* ============================================================== status == */

/* Status byte 18 — status type (SDM §4 table (5), p. 28). */
typedef enum {
    PT_STATUS_REPLY          = 0x00,
    PT_STATUS_PRINTING_DONE  = 0x01,
    PT_STATUS_ERROR_OCCURRED = 0x02,
    PT_STATUS_TURNED_OFF     = 0x04,
    PT_STATUS_NOTIFICATION   = 0x05,
    PT_STATUS_PHASE_CHANGE   = 0x06,
} pt_status_type_t;

/* Status byte 19 — phase type (SDM §4 table (6), p. 28). */
typedef enum {
    PT_PHASE_EDITING  = 0x00,  /* "reception possible" */
    PT_PHASE_PRINTING = 0x01,
} pt_phase_type_t;

/* Status byte 22 — notification number (SDM §4 table (7), p. 29). */
typedef enum {
    PT_NOTIF_NONE         = 0x00,
    PT_NOTIF_COVER_OPEN   = 0x01,
    PT_NOTIF_COVER_CLOSED = 0x02,
} pt_notification_t;

/* Decoded 32-byte status struct (SDM §4 status table, pp. 24-25). */
typedef struct {
    pt_model_t        model;
    uint8_t           error1;             /* PT_ERR1_* bit flags          */
    uint8_t           error2;             /* PT_ERR2_* bit flags          */
    uint8_t           media_width_mm;
    pt_media_type_t   media_type;
    uint8_t           mode;               /* echo of "various mode" byte  */
    pt_status_type_t  status_type;
    pt_phase_type_t   phase_type;
    uint16_t          phase_number;       /* hi=byte20, lo=byte21         */
    pt_notification_t notification;
    uint8_t           tape_color_id;      /* SDM table (8), p. 29         */
    uint8_t           text_color_id;      /* SDM table (10), p. 30        */
    uint32_t          hardware_settings;  /* bytes 26-29                  */
} pt_status_t;

/* Parse a 32-byte status buffer. Returns PT_OK and fills `out`, or:
 *   PT_ERR_BAD_LEN   if len != 32
 *   PT_ERR_BAD_MAGIC if buf[0] != 0x80 */
pt_err_t pt_status_decode(const uint8_t *buf, size_t len, pt_status_t *out);

/* ========================================================== print info == */

/* "Print information command" ESC i z — 10 bytes (SDM §4 ESC i z, p. 32). */
typedef struct {
    pt_media_type_t media_type;
    uint8_t  media_width_mm;
    uint8_t  media_length_mm;   /* 0 for continuous tape */
    uint32_t raster_lines;      /* total rows in this page (n5..n8 LE) */
    bool     is_first_page;     /* n9: false (0) for first, true (1) after */
    bool     valid_kind;        /* PI_KIND   = 0x02 in n1 */
    bool     valid_width;       /* PI_WIDTH  = 0x04 in n1 */
    bool     valid_length;      /* PI_LENGTH = 0x08 in n1 */
    bool     priority_quality;  /* PI_QUALITY = 0x40 (unused on these) */
    bool     recover_always_on; /* PI_RECOVER = 0x80 */
} pt_print_info_t;

/* ============================================================ mode bits == */

/* "Various mode settings" ESC i M (SDM §4 ESC i M, p. 33). */
typedef struct {
    bool auto_cut;        /* bit 6 */
    bool mirror_print;    /* bit 7 */
} pt_mode_settings_t;

/* "Advanced mode settings" ESC i K (SDM §4 ESC i K, p. 33). */
typedef struct {
    bool no_chain_print;  /* bit 3 — feed + cut after last copy             */
    bool special_tape;    /* bit 4 — disable cutter (fabric/iron-on)        */
    bool no_buffer_clear; /* bit 7 — keep expansion buffer between labels   */
} pt_advanced_settings_t;

/* ========================================================= compression == */

/* "Select compression mode" M (SDM §4 M, p. 35). */
typedef enum {
    PT_COMPRESSION_NONE = 0,
    PT_COMPRESSION_TIFF = 2,  /* PackBits, 16-byte raw chunks */
} pt_compression_t;

/* ========================================================= mode switch == */

/* "Switch dynamic command mode" ESC i a — must select RASTER before sending
 * raster jobs (SDM §4 ESC i a, p. 31). */
typedef enum {
    PT_CMD_MODE_ESCP        = 0,
    PT_CMD_MODE_RASTER      = 1,
    PT_CMD_MODE_PT_TEMPLATE = 3,
} pt_command_mode_t;

/* ============================================================ encoders == */

/* Each pt_encode_* writes command bytes to buf and returns the number of
 * bytes written, or a negative pt_err_t on failure. Implementations land
 * in pt700-xmg (control commands) and pt700-979 (raster + packbits). */

int pt_encode_invalidate        (uint8_t *buf, size_t cap);              /* 100x 0x00         */
int pt_encode_init              (uint8_t *buf, size_t cap);              /* ESC @             */
int pt_encode_status_request    (uint8_t *buf, size_t cap);              /* ESC i S           */
int pt_encode_switch_mode       (uint8_t *buf, size_t cap, pt_command_mode_t mode);
int pt_encode_print_info        (uint8_t *buf, size_t cap, const pt_print_info_t *info);
int pt_encode_mode_settings     (uint8_t *buf, size_t cap, const pt_mode_settings_t *m);
int pt_encode_advanced_settings (uint8_t *buf, size_t cap, const pt_advanced_settings_t *a);
int pt_encode_margin            (uint8_t *buf, size_t cap, uint16_t margin_dots);
int pt_encode_compression       (uint8_t *buf, size_t cap, pt_compression_t mode);
/* NB: emits 0x47 ('G') as the opcode; SDM v1.11 §3 says 0x67 ('g') but
 * the OSS implementations all use 0x47 and it prints — see the impl. */
int pt_encode_raster_row        (uint8_t *buf, size_t cap, const uint8_t *row, size_t row_len);
int pt_encode_zero_row          (uint8_t *buf, size_t cap);              /* Z = 0x5A          */
int pt_encode_print_page        (uint8_t *buf, size_t cap);              /* FF = 0x0C         */
int pt_encode_print_last        (uint8_t *buf, size_t cap);              /* Ctrl-Z = 0x1A     */

/* =========================================================== packbits == */

/* PackBits per SDM §4 M (p. 35). 1-byte units. Repeat encoded as negative
 * count (count = -N where N = repeats - 1). Raw chunks as positive count
 * (count = N where N = bytes - 1). Per SDM: if a raw chunk would exceed
 * 16 bytes the encoder is allowed to split the row anywhere — the printer
 * tolerates the implicit zero-padding to 16 bytes per row. */
int pt_packbits_encode(uint8_t *out, size_t cap, const uint8_t *in, size_t in_len);
int pt_packbits_decode(uint8_t *out, size_t cap, const uint8_t *in, size_t in_len);

/* ============================================================ bitmap === */

/* Convert a 1-bit bitmap (MSB-first within byte, row-major) into a sequence
 * of 16-byte raster rows, applying the tape's left/right margin pin offsets.
 *
 * bitmap_width must equal geom->print_pins for the loaded tape.
 * out must have room for bitmap_height * 16 bytes; *out_rows is set to
 * bitmap_height on success.
 *
 * No "g" framing is added here — call pt_encode_raster_row on each slice. */
pt_err_t pt_bitmap_to_raster(
    const uint8_t            *bitmap,
    size_t                    bitmap_width,
    size_t                    bitmap_height,
    const pt_tape_geometry_t *geom,
    uint8_t                  *out,
    size_t                   *out_rows);

#ifdef __cplusplus
}
#endif

#endif /* PT_PROTOCOL_H */
