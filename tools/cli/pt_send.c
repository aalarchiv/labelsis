/*
 * pt_send — Linux CLI: print a .pbm onto a USB-connected PT-* via
 *           libusb + pt_session.
 *
 * The user's existing .pbm fixtures under refs/ptouch-770/ are laid
 * out in the ptouch-770 convention: PBM "width" = number of raster
 * lines (feed direction), PBM "height" = number of pin positions
 * (cross-tape direction). The printer firmware applies the
 * left/right margin internally — the host always sends 128 pin bits
 * per raster row regardless of tape width — so this CLI pads or
 * truncates the PBM's height to exactly 128 pins and emits raw
 * raster rows without margin offset, mirroring ptouch-770.
 *
 * Thin wrapper around pt_session_print_raster: PBM parsing, optional
 * padding, libusb open/close, error reporting.
 */

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_protocol.h"
#include "pt_session.h"
#include "pt_transport.h"
#include "pt_transport_libusb.h"

#define HEAD_PINS 128

static const char *USAGE =
    "Usage: pt_send [options] <input.pbm>\n"
    "       pt_send --info\n"
    "\n"
    "Options:\n"
    "  --no-cut             disable auto-cut at end of job (use for tiny\n"
    "                       labels that might otherwise jam in the cutter)\n"
    "  --chain              chain printing — skip the trailing feed+cut so\n"
    "                       the NEXT job continues directly on this tape\n"
    "                       (saves the ~25 mm leader the printer normally\n"
    "                       wastes between cuts). The last job in a series\n"
    "                       should NOT pass --chain so the strip is released.\n"
    "  --mirror             mirror-print (for transparent tape backside)\n"
    "  --no-compression     send raster rows raw (no PackBits)\n"
    "  --margin=DOTS        leading margin in dots (default 14 ≈ 2 mm)\n"
    "  --width=MM           require this tape width (default: trust loaded tape)\n"
    "  -v, --verbose        log every status message the printer emits\n"
    "      --info           probe the connected printer + tape and exit\n"
    "  -h, --help           show this help\n";

/* ----------------------------------------------------------- PBM reader */

typedef struct {
    int      width;   /* feed direction (raster lines)        */
    int      height;  /* pin direction                        */
    uint8_t *bytes;   /* width-major: width * row_bytes total */
} pbm_t;

/* Read a PGM/PBM-style ASCII token (number or 'P4' header), skipping
 * whitespace and # comments. Returns the integer or -1 on EOF/error. */
static int read_int_token(FILE *f)
{
    int c;
    for (;;) {
        c = fgetc(f);
        if (c == EOF) return -1;
        if (c == '#') { while ((c = fgetc(f)) != '\n' && c != EOF); continue; }
        if (!isspace(c)) break;
    }
    if (!isdigit(c)) { ungetc(c, f); return -1; }
    int v = 0;
    while (isdigit(c)) { v = v * 10 + (c - '0'); c = fgetc(f); }
    if (c != EOF) ungetc(c, f);
    return v;
}

/* Load a P4 (binary) PBM. PBM convention: width = pixels per row,
 * height = rows. We don't transpose — pad_to_128_pins below interprets
 * PBM "height" as the pin direction, matching ptouch-770. */
static int pbm_load(const char *path, pbm_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "pt_send: %s: %s\n", path, strerror(errno)); return -1; }
    int magic_p = fgetc(f);
    int magic_n = fgetc(f);
    if (magic_p != 'P' || magic_n != '4') {
        fprintf(stderr, "pt_send: %s: not a P4 PBM\n", path);
        fclose(f); return -1;
    }
    int w = read_int_token(f);
    int h = read_int_token(f);
    if (w <= 0 || h <= 0) {
        fprintf(stderr, "pt_send: %s: bad dimensions\n", path);
        fclose(f); return -1;
    }
    /* exactly one whitespace separator between header and data */
    fgetc(f);

    int row_bytes = (w + 7) / 8;
    size_t n = (size_t)row_bytes * (size_t)h;
    uint8_t *buf = malloc(n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, n, f) != n) {
        fprintf(stderr, "pt_send: %s: short read\n", path);
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    out->width = w; out->height = h; out->bytes = buf;
    return 0;
}

/* Build a (HEAD_PINS × raster_lines) buffer in the layout
 * pt_bitmap_to_raster_no_margin() consumes — but since we want to skip
 * margin offsets entirely (printer applies them), we produce 128 raster
 * rows of 16 bytes each directly here.
 *
 * Output: raster_lines × 16 bytes. For each raster line l (0..w-1):
 *   pin p (0..127): set if PBM pixel (col=l, row=p) is set in src,
 *                   centred when src->height < 128.
 */
static uint8_t *render_to_raster_rows(const pbm_t *src, size_t *out_rows)
{
    int raster_lines = src->width;
    int src_h        = src->height;
    int row_bytes    = (src->width + 7) / 8;

    uint8_t *out = calloc((size_t)raster_lines, 16);
    if (!out) return NULL;

    /* Centre a smaller PBM on the head; truncate a larger one. */
    int top_pad = (src_h < HEAD_PINS) ? (HEAD_PINS - src_h) / 2 : 0;
    int copy_h  = (src_h < HEAD_PINS) ? src_h : HEAD_PINS;
    int top_src = (src_h > HEAD_PINS) ? (src_h - HEAD_PINS) / 2 : 0;

    for (int l = 0; l < raster_lines; l++) {
        uint8_t *row = out + l * 16;
        for (int p = 0; p < copy_h; p++) {
            int sy = top_src + p;
            int dy = top_pad + p;
            uint8_t b = src->bytes[sy * row_bytes + l / 8];
            if (b & (uint8_t)(0x80 >> (l % 8))) {
                row[dy / 8] |= (uint8_t)(0x80 >> (dy % 8));
            }
        }
    }
    *out_rows = (size_t)raster_lines;
    return out;
}

/* ----------------------------------------------------------- verbose log */

static const char *status_type_name(pt_status_type_t t)
{
    switch (t) {
    case PT_STATUS_REPLY:          return "reply";
    case PT_STATUS_PRINTING_DONE:  return "printing-done";
    case PT_STATUS_ERROR_OCCURRED: return "error";
    case PT_STATUS_TURNED_OFF:     return "turned-off";
    case PT_STATUS_NOTIFICATION:   return "notification";
    case PT_STATUS_PHASE_CHANGE:   return "phase-change";
    }
    return "?";
}

static void log_status(const pt_status_t *s, void *unused)
{
    (void)unused;
    fprintf(stderr,
        "  status: type=%-13s phase=%s phase_num=%u "
        "err1=0x%02x err2=0x%02x notif=%u media=%u/0x%02x\n",
        status_type_name(s->status_type),
        s->phase_type == PT_PHASE_PRINTING ? "printing" : "editing",
        s->phase_number, s->error1, s->error2, s->notification,
        s->media_width_mm, s->media_type);
}

/* ----------------------------------------------------------- error map */

static const char *err_str(pt_err_t e)
{
    switch (e) {
    case PT_OK: return "ok";
    case PT_ERR_BUF_TOO_SMALL:  return "internal buffer too small";
    case PT_ERR_INVALID_ARG:    return "invalid argument";
    case PT_ERR_BAD_LEN:        return "bad status length";
    case PT_ERR_BAD_MAGIC:      return "bad status magic";
    case PT_ERR_TRANSPORT:      return "USB transport failure";
    case PT_ERR_TIMEOUT:        return "timed out waiting for printer";
    case PT_ERR_NO_MEDIA:       return "no tape loaded";
    case PT_ERR_CUTTER_JAM:     return "cutter jam";
    case PT_ERR_WEAK_BATTERY:   return "battery low";
    case PT_ERR_HIGH_VOLTAGE:   return "high-voltage AC adapter required";
    case PT_ERR_REPLACE_MEDIA:  return "replace tape (wrong media)";
    case PT_ERR_COVER_OPEN:     return "cover open";
    case PT_ERR_OVERHEAT:       return "print head overheated";
    case PT_ERR_MEDIA_MISMATCH: return "loaded tape does not match --width";
    }
    return "unknown error";
}

/* ----------------------------------------------------------- main */

int main(int argc, char **argv)
{
    pt_session_options_t opts;
    pt_session_options_default(&opts);

    int  width_override = 0;  /* 0 = auto */

    bool info_only = false;
    static const struct option longopts[] = {
        { "no-cut",         no_argument,       0, 'C' },
        { "chain",          no_argument,       0, 'N' },
        { "mirror",         no_argument,       0, 'M' },
        { "no-compression", no_argument,       0, 'R' },
        { "margin",         required_argument, 0, 'D' },
        { "width",          required_argument, 0, 'W' },
        { "verbose",        no_argument,       0, 'v' },
        { "info",           no_argument,       0, 'I' },
        { "help",           no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "vh", longopts, NULL)) != -1) {
        switch (c) {
        case 'C': opts.auto_cut = false; break;
        case 'N': opts.no_chain_print = false; break;
        case 'M': opts.mirror_print = true; break;
        case 'R': opts.compression = PT_COMPRESSION_NONE; break;
        case 'D': opts.margin_dots = (uint16_t)atoi(optarg); break;
        case 'W': width_override = atoi(optarg); break;
        case 'v': opts.on_status = log_status; break;
        case 'I': info_only = true; break;
        case 'h': fputs(USAGE, stdout); return 0;
        default:  fputs(USAGE, stderr); return 2;
        }
    }

    if (info_only) {
        pt_transport_libusb_t *u = pt_transport_libusb_open();
        if (!u) {
            fprintf(stderr, "pt_send: no PT-* found on USB.\n");
            return 1;
        }
        pt_transport_t t = pt_transport_libusb_transport(u);
        pt_status_t st;
        if (pt_session_query_status(&t, &st, NULL) != PT_OK) {
            fprintf(stderr, "pt_send: status query failed\n");
            pt_transport_libusb_close(u);
            return 1;
        }
        printf("Connected:\n");
        printf("  model            0x%02x\n", st.model);
        printf("  status type      0x%02x\n", st.status_type);
        printf("  error1 / error2  0x%02x / 0x%02x\n", st.error1, st.error2);
        printf("Loaded tape:\n");
        printf("  width            %u mm (status byte 10)\n", st.media_width_mm);
        printf("  type             0x%02x\n", st.media_type);
        printf("  tape colour      0x%02x\n", st.tape_color_id);
        printf("  text colour      0x%02x\n", st.text_color_id);

        pt_tape_geometry_t g;
        if (pt_tape_geometry_tze(st.media_width_mm, &g) == PT_OK) {
            double mm_per_dot = 25.4 / 180.0;
            uint8_t off = (g.tape_width_dots - g.print_pins) / 2;
            printf("Geometry @ 180 dpi (SDM pp. 14, 20):\n");
            printf("  print head       %u dots\n", g.total_pins);
            printf("  tape width       %u dots (%.2f mm)\n",
                   g.tape_width_dots, g.tape_width_dots * mm_per_dot);
            printf("  print width      %u dots (%.2f mm)\n",
                   g.print_pins, g.print_pins * mm_per_dot);
            printf("  margin per side  %u dots (%.2f mm) of tape "
                   "physically not addressable\n",
                   off, off * mm_per_dot);
            printf("  head left/right  %u / %u pins masked\n",
                   g.left_margin_pins, g.right_margin_pins);
        }
        pt_transport_libusb_close(u);
        return 0;
    }

    if (optind >= argc) { fputs(USAGE, stderr); return 2; }
    const char *pbm_path = argv[optind];

    pbm_t pbm;
    if (pbm_load(pbm_path, &pbm) != 0) return 1;

    size_t   n_rows = 0;
    uint8_t *rows   = render_to_raster_rows(&pbm, &n_rows);
    free(pbm.bytes);
    if (!rows) {
        fprintf(stderr, "pt_send: out of memory rendering %s\n", pbm_path);
        return 1;
    }

    pt_transport_libusb_t *u = pt_transport_libusb_open();
    if (!u) {
        fprintf(stderr, "pt_send: no PT-* found on USB. "
                        "Is it powered on and connected?\n");
        free(rows);
        return 1;
    }
    pt_transport_t t = pt_transport_libusb_transport(u);

    /* Tape-fit check: if the PBM height exceeds the loaded tape's print
     * pin count, the rows above and below the print area land in the
     * margins and silently won't fire. Warn so the user can swap to a
     * smaller PBM or wider tape rather than being surprised by clipping. */
    pt_status_t st;
    pt_err_t qerr = pt_session_query_status(&t, &st, NULL);
    if (qerr == PT_OK) {
        pt_tape_geometry_t g;
        if (pt_tape_geometry_tze(st.media_width_mm, &g) == PT_OK
            && pbm.height > g.print_pins) {
            fprintf(stderr,
                    "pt_send: WARNING: PBM is %d px tall but loaded %u mm tape "
                    "has only %u print pins; top/bottom rows will be clipped to "
                    "the margins. Use a %u-px-tall PBM for full coverage.\n",
                    pbm.height, st.media_width_mm, g.print_pins, g.print_pins);
        }
    }

    fprintf(stderr, "pt_send: rendering %d × %d PBM as %zu raster rows\n",
            pbm.width, pbm.height, n_rows);

    pt_err_t err = pt_session_print_raster(&t, rows, n_rows,
                                           (uint8_t)width_override, &opts);
    pt_transport_libusb_close(u);
    free(rows);

    if (err != PT_OK) {
        fprintf(stderr, "pt_send: print failed (%d): %s\n", err, err_str(err));
        return 1;
    }
    fprintf(stderr, "pt_send: done\n");
    return 0;
}
