/* pt_bitmap_to_raster: verify pixel-to-pin mapping for several tapes. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_protocol.h"

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); exit(1); } \
} while (0)

/* 24 mm tape: print_pins=128, no margin. A single-pixel-wide bitmap with
 * pixel (row=0, col=0)=1 should set bit 7 of byte 0 of raster row 0. */
static void test_24mm_single_pixel(void)
{
    pt_tape_geometry_t g;
    EXPECT(pt_tape_geometry_tze(24, &g) == PT_OK, "24mm geometry");

    /* 128 pixels across × 1 raster line, all zero except row=0 col=0. */
    uint8_t bitmap[128 * 1] = {0};
    bitmap[0] = 0x80;  /* row=0 bit set */

    uint8_t out[16] = {0};
    size_t rows = 0;
    EXPECT(pt_bitmap_to_raster(bitmap, 128, 1, &g, out, &rows) == PT_OK,
           "convert");
    EXPECT(rows == 1, "one raster row");
    EXPECT(out[0] == 0x80, "pin 0 set in byte 0");
    for (size_t i = 1; i < 16; i++) EXPECT(out[i] == 0x00, "rest zero");
}

/* 24 mm tape: pixel (row=127, col=0) → pin 127 → bit 0 of byte 15. */
static void test_24mm_last_pin(void)
{
    pt_tape_geometry_t g; pt_tape_geometry_tze(24, &g);
    uint8_t bitmap[128] = {0};
    bitmap[127] = 0x80;
    uint8_t out[16] = {0};
    size_t rows;
    EXPECT(pt_bitmap_to_raster(bitmap, 128, 1, &g, out, &rows) == PT_OK,
           "convert");
    EXPECT(out[15] == 0x01, "pin 127 = byte 15 bit 0");
    for (size_t i = 0; i < 15; i++) EXPECT(out[i] == 0x00, "rest zero");
}

/* 12 mm tape: print_pins=70, left_margin=29. Pixel (row=0, col=0) should
 * map to pin 29 → bit (7 - 29%8) = bit 2 of byte 3. */
static void test_12mm_margin_offset(void)
{
    pt_tape_geometry_t g;
    EXPECT(pt_tape_geometry_tze(12, &g) == PT_OK, "12mm geometry");
    EXPECT(g.print_pins == 70 && g.left_margin_pins == 29, "12mm pins");

    uint8_t bitmap[70] = {0};
    bitmap[0] = 0x80;  /* row=0 bit set */
    uint8_t out[16] = {0};
    size_t rows;
    EXPECT(pt_bitmap_to_raster(bitmap, 70, 1, &g, out, &rows) == PT_OK,
           "convert");
    EXPECT(out[3] == (uint8_t)(0x80 >> (29 % 8)),
           "pin 29 -> byte 3 bit (7-29%8)");
    /* Margin pins (0..28) and right margin (99..127) must be zero. */
    EXPECT(out[0] == 0 && out[1] == 0 && out[2] == 0,
           "left margin bytes zero");
    EXPECT(out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 0,
           "right margin bytes zero");
}

/* Multi-line: a 24×2 bitmap with two distinct columns. */
static void test_two_lines(void)
{
    pt_tape_geometry_t g; pt_tape_geometry_tze(24, &g);
    /* row_stride = (raster_lines + 7) / 8 = 1 byte per row.
     * Set bitmap[row=0][col=0]=1, bitmap[row=0][col=1]=0,
     *     bitmap[row=1][col=0]=0, bitmap[row=1][col=1]=1. */
    uint8_t bitmap[128] = {0};
    bitmap[0] = 0x80;  /* row 0: col 0 bit (MSB) */
    bitmap[1] = 0x40;  /* row 1: col 1 bit (next-MSB) */

    uint8_t out[32] = {0};
    size_t rows;
    EXPECT(pt_bitmap_to_raster(bitmap, 128, 2, &g, out, &rows) == PT_OK,
           "convert 2 lines");
    EXPECT(rows == 2, "two rows out");
    /* Raster row 0: pin 0 set. Raster row 1: pin 1 set. */
    EXPECT(out[0] == 0x80, "row 0: pin 0");
    EXPECT(out[16] == 0x40, "row 1: pin 1");
}

static void test_bad_inputs(void)
{
    pt_tape_geometry_t g; pt_tape_geometry_tze(12, &g);
    uint8_t bitmap[16] = {0}, out[16];
    size_t rows;
    /* pixels_across must equal print_pins (70). */
    EXPECT(pt_bitmap_to_raster(bitmap, 64, 1, &g, out, &rows)
           == PT_ERR_INVALID_ARG, "wrong width");
    EXPECT(pt_bitmap_to_raster(NULL, 70, 1, &g, out, &rows)
           == PT_ERR_INVALID_ARG, "null bitmap");
}

int main(void)
{
    test_24mm_single_pixel();
    test_24mm_last_pin();
    test_12mm_margin_offset();
    test_two_lines();
    test_bad_inputs();
    printf("bitmap: all tests passed\n");
    return 0;
}
