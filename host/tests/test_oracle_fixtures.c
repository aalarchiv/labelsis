/* Structural sanity check on captured oracle fixtures.
 *
 * Verifies that every fixture in test/fixtures/ begins with the canonical
 * PT-* job prefix (100x 0x00 invalidate + ESC @ + ESC i a 0x01 + ESC i z)
 * and ends with the print-with-feed byte (0x1A). Full byte-for-byte diff
 * against pt_protocol's encoder output lands in a follow-up issue once
 * the encoder itself exists. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture.h"

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } \
} while (0)

static void check_prefix(const char *name, const fixture_t *f)
{
    EXPECT(f->len > 102 + 4 + 28, "fixture too short for full job prefix");

    for (size_t i = 0; i < 100; i++)
        EXPECT(f->data[i] == 0x00, "first 100 bytes must be invalidate (0x00)");

    EXPECT(f->data[100] == 0x1b && f->data[101] == 0x40,
           "bytes 100-101 must be ESC @ (init)");

    EXPECT(f->data[102] == 0x1b && f->data[103] == 0x69 &&
           f->data[104] == 0x61 && f->data[105] == 0x01,
           "bytes 102-105 must be ESC i a 0x01 (switch to raster)");

    EXPECT(f->data[106] == 0x1b && f->data[107] == 0x69 &&
           f->data[108] == 0x7a,
           "bytes 106-108 must be ESC i z (print info command)");

    EXPECT(f->data[f->len - 1] == 0x1a,
           "last byte must be 0x1A (print + feed)");

    printf("  ok: %-40s %zu bytes\n", name, f->len);
}

static const char *FIXTURES[] = {
    "tux-128px-bw_24mm.bin",
    "tux-128px-bw_24mm_raw.bin",
    "calibrate_12mm_tape_12mm.bin",
    "calibrate_12mm_tape_12mm_raw.bin",
    "gunda_18mm_18mm.bin",
    "l_24mm_long.bin",
};

int main(void)
{
    char path[1024];
    for (size_t i = 0; i < sizeof FIXTURES / sizeof FIXTURES[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", fixture_dir(), FIXTURES[i]);
        fixture_t f;
        if (fixture_load(path, &f) != 0) {
            fprintf(stderr, "FAIL: could not load %s\n", path);
            return 1;
        }
        check_prefix(FIXTURES[i], &f);
        fixture_free(&f);
    }
    return 0;
}
