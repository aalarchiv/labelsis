/* Encoder vs. oracle semantic diff.
 *
 * For each captured fixture:
 *   1. Walk the byte stream parsing commands; identify the active
 *      compression mode and the raster rows.
 *   2. Recover each 16-byte row (raw payload, or PackBits-decoded).
 *   3. Re-encode each row through our pt_protocol's PackBits encoder.
 *   4. Decode our output and assert it equals the original row.
 *
 * This proves two things at once:
 *   a) pt_packbits_decode reads any spec-valid PackBits the oracle emits
 *      (covers raw-mode 0x10/0x00 framing AND PackBits with the oracle's
 *      choices of run/raw chunks).
 *   b) Our pt_packbits_encode + pt_packbits_decode round-trip is loss-
 *      less for the kinds of row data the oracle produces.
 *
 * Direct byte-for-byte diff between our encoder output and the oracle
 * is intentionally not done — both implementations make different
 * (spec-valid) choices on n=2 repeats and chunk sizes; see bd memory
 * "pt700 PACKBITS DIVERGENCE". */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"
#include "pt_protocol.h"

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); exit(1); } \
} while (0)

/* Walk a captured job byte stream, calling `on_row` for each 16-byte
 * raster row recovered. Returns 0 on success, or the negative offset
 * where parsing failed (so the test can log it). */
static int walk_job(const uint8_t *buf, size_t len,
                    void (*on_row)(const uint8_t row[16], void *ctx),
                    void *ctx)
{
    bool compressed = false;  /* updated by the M command */
    size_t i = 0;
    while (i < len) {
        uint8_t op = buf[i];
        if (op == 0x00) { i++; continue; }       /* NULL invalidate filler */
        if (op == 0x1b) {                        /* ESC sequence */
            if (i + 1 >= len) return -(int)i - 1;
            uint8_t op2 = buf[i + 1];
            if (op2 == 0x40) { i += 2; continue; }              /* ESC @ */
            if (op2 != 0x69) return -(int)i - 1;
            if (i + 2 >= len) return -(int)i - 1;
            switch (buf[i + 2]) {
            case 'S': i += 3; break;
            case 'a': i += 4; break;
            case 'z': i += 13; break;
            case 'M': i += 4; break;
            case 'K': i += 4; break;
            case 'd': i += 5; break;
            default: return -(int)i - 1;
            }
            continue;
        }
        if (op == 0x4d) {                        /* M {n} */
            if (i + 1 >= len) return -(int)i - 1;
            compressed = (buf[i + 1] == 0x02);
            i += 2;
            continue;
        }
        if (op == 0x47) {                        /* G {lo}{hi}{data} */
            if (i + 3 > len) return -(int)i - 1;
            size_t pl = (size_t)buf[i + 1] | ((size_t)buf[i + 2] << 8);
            if (i + 3 + pl > len) return -(int)i - 1;
            uint8_t row[16] = {0};
            if (compressed) {
                int n = pt_packbits_decode(row, sizeof row,
                                           buf + i + 3, pl);
                if (n != 16) return -(int)i - 1;
            } else {
                if (pl != 16) return -(int)i - 1;
                memcpy(row, buf + i + 3, 16);
            }
            on_row(row, ctx);
            i += 3 + pl;
            continue;
        }
        if (op == 0x5a) {                        /* Z (zero row) */
            uint8_t zero[16] = {0};
            on_row(zero, ctx);
            i++;
            continue;
        }
        if (op == 0x0c || op == 0x1a) { i++; continue; }  /* page / +feed */
        return -(int)i - 1;                       /* unknown opcode */
    }
    return 0;
}

struct row_check_ctx { size_t rows_seen; size_t rows_failed; };

static void roundtrip_row(const uint8_t row[16], void *vctx)
{
    struct row_check_ctx *ctx = vctx;
    ctx->rows_seen++;

    /* Re-encode the row through our PackBits, then decode it back, and
     * assert the round-trip preserves the data. */
    uint8_t enc[64];
    int n = pt_packbits_encode(enc, sizeof enc, row, 16);
    if (n < 0) { ctx->rows_failed++; return; }

    uint8_t dec[16];
    int m = pt_packbits_decode(dec, sizeof dec, enc, (size_t)n);
    if (m != 16 || memcmp(dec, row, 16) != 0) ctx->rows_failed++;
}

struct fixture_spec {
    const char *name;
    uint8_t     width_mm;   /* informational only */
    bool        compressed;
};

static const struct fixture_spec FIXTURES[] = {
    { "tux-128px-bw_24mm.bin",          24, true  },
    { "tux-128px-bw_24mm_raw.bin",      24, false },
    { "calibrate_12mm_tape_12mm.bin",   12, true  },
    { "calibrate_12mm_tape_12mm_raw.bin", 12, false },
    { "gunda_18mm_18mm.bin",            18, true  },
    { "l_24mm_long.bin",                24, true  },
};

int main(void)
{
    char path[1024];
    for (size_t i = 0; i < sizeof FIXTURES / sizeof FIXTURES[0]; i++) {
        const struct fixture_spec *f = &FIXTURES[i];
        snprintf(path, sizeof path, "%s/%s", fixture_dir(), f->name);
        fixture_t fx;
        EXPECT(fixture_load(path, &fx) == 0, path);

        struct row_check_ctx ctx = {0};
        int rc = walk_job(fx.data, fx.len, roundtrip_row, &ctx);
        if (rc != 0) {
            size_t fail_off = (size_t)(-rc - 1);
            fprintf(stderr,
                    "FAIL: %s — parse failed at offset %zu (byte 0x%02x)\n  context:",
                    f->name, fail_off, fx.data[fail_off]);
            for (size_t k = (fail_off > 8 ? fail_off - 8 : 0);
                 k < fail_off + 8 && k < fx.len; k++)
                fprintf(stderr, " %02x", fx.data[k]);
            fprintf(stderr, "\n");
            exit(1);
        }
        EXPECT(ctx.rows_seen > 0, "fixture has raster rows");
        EXPECT(ctx.rows_failed == 0, "every row round-trips losslessly");

        printf("  ok: %-40s %zu rows (%s)\n",
               f->name, ctx.rows_seen, f->compressed ? "packbits" : "raw");
        fixture_free(&fx);
    }
    return 0;
}
