/*
 * ptouch-770-capture - derived from ptouch-770-write.c.
 *
 * Upstream: refs/ptouch-770/ptouch-770-write.c (GPL-2.0+).
 *
 * Modifications relative to upstream:
 *   - No udev / device discovery: writes the would-be USB byte stream to a
 *     regular file path passed on the command line.
 *   - Status reads stubbed out: get_printer_status() is replaced by a
 *     no-op that returns a caller-supplied media_width - there is no
 *     printer on the other end of the file descriptor.
 *   - Optional --no-compression flag: emits raw 16-byte rasters via
 *     `g 0x10 0x00 <data>` instead of the PackBits-compressed `g <len_lo>
 *     <len_hi> <packbits>` so we get fixtures with M 0x00 baseline too.
 *   - Removed the (always-failing-on-capture) status reads between rows.
 *
 * The bytes this program emits are identical to those upstream would have
 * sent over USB given the same .pbm input, tape width, and compression
 * mode. Captured output is committed to test/fixtures/ as the oracle for
 * pt_protocol encoder round-trip tests.
 *
 * Tools-only: never compiled into firmware. License remains GPL-2.0+.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define COL_HEIGHT 16  /* 128 pins / 8 bits */

/* read_pbm_file: copied verbatim from upstream so column packing is
 * byte-for-byte identical. */
static int read_pbm_file(FILE *f, unsigned char **ptr)
{
    enum {
        STATE_PBM_FAIL, STATE_PBM_INIT, STATE_PBM_TYPE,
        STATE_PBM_DIM_INIT, STATE_PBM_DIM_FIRST_NUM,
        STATE_PBM_DIM_FIRST_DONE, STATE_PBM_DIM_SECOND_NUM,
        STATE_PBM_DATA_ASCII, STATE_PBM_DATA_BIN, STATE_PBM_FINISHED
    };
    int c, state = STATE_PBM_INIT;
    int pbm_type = 0, pbm_dim_value = 0, pbm_x_dim = 0, pbm_y_dim = 0;
    int pbm_x = 0, pbm_y = 0, bitmap_array_size = -1;
    unsigned char *bitmap_array = NULL;

    while (state != STATE_PBM_FINISHED && (c = fgetc(f)) >= 0) {
        if (c == '#' && state != STATE_PBM_DATA_BIN) {
            do { c = fgetc(f); } while (c != '\n' && c >= 0);
        }
        switch (state) {
        case STATE_PBM_INIT:
            state = (c == 'P') ? STATE_PBM_TYPE : STATE_PBM_FAIL;
            if (state == STATE_PBM_TYPE) pbm_type = 0;
            break;
        case STATE_PBM_TYPE:
            if (c <= ' ') {
                state = (pbm_type == 1 || pbm_type == 4)
                    ? STATE_PBM_DIM_INIT : STATE_PBM_FAIL;
                pbm_dim_value = 0;
            } else if (c < '0' || c > '9') {
                state = STATE_PBM_FAIL;
            } else {
                pbm_type = pbm_type * 10 + (c - '0');
            }
            break;
        case STATE_PBM_DIM_INIT:
        case STATE_PBM_DIM_FIRST_NUM:
        case STATE_PBM_DIM_FIRST_DONE:
        case STATE_PBM_DIM_SECOND_NUM:
            if (c <= ' ') {
                if (state == STATE_PBM_DIM_SECOND_NUM) {
                    if (pbm_dim_value <= 0) { state = STATE_PBM_FAIL; }
                    else {
                        pbm_y_dim = pbm_dim_value;
                        bitmap_array_size = COL_HEIGHT * pbm_x_dim;
                        bitmap_array = calloc(1, bitmap_array_size);
                        if (!bitmap_array) state = STATE_PBM_FAIL;
                        else {
                            pbm_x = 0; pbm_y = 0;
                            state = (pbm_type == 1)
                                ? STATE_PBM_DATA_ASCII : STATE_PBM_DATA_BIN;
                        }
                    }
                } else if (state == STATE_PBM_DIM_FIRST_NUM) {
                    if (pbm_dim_value <= 0) state = STATE_PBM_FAIL;
                    else {
                        pbm_x_dim = pbm_dim_value;
                        state = STATE_PBM_DIM_FIRST_DONE;
                        pbm_dim_value = 0;
                    }
                }
            } else if (c < '0' || c > '9') {
                state = STATE_PBM_FAIL;
            } else {
                if (state == STATE_PBM_DIM_INIT)
                    state = STATE_PBM_DIM_FIRST_NUM;
                else if (state == STATE_PBM_DIM_FIRST_DONE)
                    state = STATE_PBM_DIM_SECOND_NUM;
                pbm_dim_value = pbm_dim_value * 10 + (c - '0');
            }
            break;
        case STATE_PBM_DATA_ASCII:
            if (c == '0' || c == '1') {
                if (pbm_x < pbm_x_dim && pbm_y < COL_HEIGHT * 8) {
                    if (c == '1')
                        bitmap_array[pbm_x * COL_HEIGHT + pbm_y / 8]
                            |= 1 << (7 - pbm_y % 8);
                }
                pbm_x++;
                if (pbm_x >= pbm_x_dim) {
                    pbm_x = 0; pbm_y++;
                    if (pbm_y >= pbm_y_dim) state = STATE_PBM_FINISHED;
                }
            }
            break;
        case STATE_PBM_DATA_BIN:
            for (int i = 0; i < 8; i++) {
                if ((pbm_x + i) < pbm_x_dim && pbm_y < COL_HEIGHT * 8) {
                    if (c & (0x80 >> i))
                        bitmap_array[(pbm_x + i) * COL_HEIGHT + pbm_y / 8]
                            |= 1 << (7 - pbm_y % 8);
                }
            }
            pbm_x += 8;
            if (pbm_x >= pbm_x_dim) {
                pbm_x = 0; pbm_y++;
                if (pbm_y >= pbm_y_dim) state = STATE_PBM_FINISHED;
            }
            break;
        default:
            free(bitmap_array);
            return -1;
        }
        if (state == STATE_PBM_FAIL) { free(bitmap_array); return -1; }
    }
    *ptr = bitmap_array;
    return bitmap_array_size;
}

static int write_persist(int h, const void *buffer, size_t size)
{
    size_t offset = 0;
    while (offset < size) {
        ssize_t l = write(h, (const char *)buffer + offset, size - offset);
        if (l < 0 && (errno == EINTR || errno == EAGAIN)) l = 0;
        if (l < 0) return -1;
        offset += l;
    }
    return (int)offset;
}

/* PackBits encoder copied from upstream - same byte output. */
static int write_rle(int h, const unsigned char *data, size_t size)
{
    /* Empty column shorthand: 0x5A. */
    static const unsigned char zero16[16] = {0};
    if (size == 16 && !memcmp(data, zero16, 16))
        return write_persist(h, "\x5a", 1) == 1 ? 0 : -1;

    unsigned char buffer[3 + 32];  /* worst case: 16 raw bytes + framing */
    buffer[0] = 0x47;              /* 'g' */
    unsigned char *w = buffer + 3;
    const unsigned char *r_start = data;
    while ((size_t)(r_start - data) < size) {
        const unsigned char *r = r_start + 1;
        while ((size_t)(r - data) < size
               && (r - r_start) < COL_HEIGHT
               && *r == *r_start) r++;
        int n = (int)(r - r_start);
        if (n > 2) {
            *w++ = (unsigned char)(1 - (signed char)n);
            *w++ = *r_start;
        } else {
            while ((size_t)(r - data) < size
                   && (r - r_start) < COL_HEIGHT
                   && *r != r[1]) r++;
            n = (int)(r - r_start);
            *w++ = (unsigned char)(n - 1);
            for (int i = 0; i < n; i++) *w++ = r_start[i];
        }
        r_start = r;
    }
    int payload = (int)(w - buffer) - 3;
    buffer[1] = (unsigned char)(payload & 0xff);
    buffer[2] = (unsigned char)((payload >> 8) & 0xff);
    return write_persist(h, buffer, w - buffer) == (int)(w - buffer) ? 0 : -1;
}

/* Uncompressed variant: emit `g 0x10 0x00 <16 raw bytes>` per row. */
static int write_raw(int h, const unsigned char *data)
{
    unsigned char framed[3 + 16];
    framed[0] = 0x47;
    framed[1] = 0x10;
    framed[2] = 0x00;
    memcpy(framed + 3, data, 16);
    return write_persist(h, framed, sizeof framed) == sizeof framed ? 0 : -1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s <input.pbm> <output.bin> <media_width_mm> [--no-compression]\n",
            prog);
}

int main(int argc, char **argv)
{
    if (argc < 4) { usage(argv[0]); return 1; }
    const char *pbm_path = argv[1];
    const char *out_path = argv[2];
    int media_width = atoi(argv[3]);
    bool compress = true;
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--no-compression")) compress = false;
        else { usage(argv[0]); return 1; }
    }
    if (media_width < 4 || media_width > 24) {
        fprintf(stderr, "media_width must be 4..24 mm\n");
        return 1;
    }

    FILE *f = fopen(pbm_path, "rb");
    if (!f) { perror(pbm_path); return 1; }
    unsigned char *bitmap = NULL;
    int bytes = read_pbm_file(f, &bitmap);
    fclose(f);
    if (bytes < 0) { fprintf(stderr, "bad pbm: %s\n", pbm_path); return 1; }

    int h = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (h < 0) { perror(out_path); free(bitmap); return 1; }

    /* Init: 100x 0x00 + ESC @. (Upstream allocates 102 bytes total.) */
    unsigned char init_buf[102];
    memset(init_buf, 0, 100);
    init_buf[100] = 0x1b;
    init_buf[101] = 0x40;
    if (write_persist(h, init_buf, 102) != 102) goto fail;

    /* No status read here - bytes match upstream because the read does
     * not produce output of its own. */

    /* Switch dynamic mode → raster. */
    if (write_persist(h, "\x1b\x69\x61\x01", 4) != 4) goto fail;

    /* The 28-byte init burst upstream sends, with our compression toggle:
     *   bytes  0-12: ESC i z print info (13 bytes)
     *   bytes 13-16: 4x 0x00 (no ESC i M emitted upstream)
     *   bytes 17-20: ESC i K 0x08 (no chain)
     *   bytes 21-25: ESC i d 0x20 0x00 (32-dot margin)
     *   bytes 26-27: M 0x02 (PackBits)  OR  M 0x00 (no compression)
     */
    unsigned char init28[28] = {0};
    memcpy(init28, "\x1b\x69\x7a\x84", 4);
    init28[4] = 0;                                  /* media type */
    init28[5] = (unsigned char)media_width;         /* media width mm */
    init28[6] = 0;                                  /* media length */
    init28[7]  = (bytes / COL_HEIGHT)        & 0xff;
    init28[8]  = (bytes / COL_HEIGHT >> 8)   & 0xff;
    init28[9]  = (bytes / COL_HEIGHT >> 16)  & 0xff;
    init28[10] = (bytes / COL_HEIGHT >> 24)  & 0xff;
    init28[11] = 0;                                 /* starting page */
    init28[12] = 0;
    /* 13..16 zero */
    memcpy(init28 + 17, "\x1b\x69\x4b\x08", 4);     /* no chain */
    memcpy(init28 + 21, "\x1b\x69\x64\x20\x00", 5); /* margin 32 dots */
    init28[26] = 0x4d;
    init28[27] = compress ? 0x02 : 0x00;
    if (write_persist(h, init28, 28) != 28) goto fail;

    /* Raster rows. */
    for (int i = 0; i < bytes; i += 16) {
        int r = compress ? write_rle(h, bitmap + i, 16)
                         : write_raw(h, bitmap + i);
        if (r) goto fail;
    }

    /* Print + feed. */
    if (write_persist(h, "\x1a", 1) != 1) goto fail;

    free(bitmap);
    close(h);
    return 0;

fail:
    perror("write");
    free(bitmap);
    close(h);
    return 1;
}
