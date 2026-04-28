/* Per-command byte assertions against the SDM v1.11 reference. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_protocol.h"

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); exit(1); } \
} while (0)

#define EXPECT_BYTES(buf, expected, n, msg) do { \
    if (memcmp((buf), (expected), (n)) != 0) { \
        fprintf(stderr, "FAIL %s:%d: %s\n  got: ", __FILE__, __LINE__, msg); \
        for (size_t _i = 0; _i < (n); _i++) fprintf(stderr, "%02x ", (buf)[_i]); \
        fprintf(stderr, "\n  exp: "); \
        for (size_t _i = 0; _i < (n); _i++) fprintf(stderr, "%02x ", (expected)[_i]); \
        fprintf(stderr, "\n"); \
        exit(1); \
    } \
} while (0)

static void test_invalidate(void)
{
    uint8_t buf[100] = {0xff};  /* poison */
    int n = pt_encode_invalidate(buf, sizeof buf);
    EXPECT(n == 100, "invalidate length");
    static const uint8_t zero[100] = {0};
    EXPECT_BYTES(buf, zero, 100, "invalidate must be 100 zero bytes");

    EXPECT(pt_encode_invalidate(buf, 99) == PT_ERR_BUF_TOO_SMALL, "buf-too-small");
}

static void test_init(void)
{
    uint8_t buf[4] = {0};
    int n = pt_encode_init(buf, sizeof buf);
    EXPECT(n == 2, "init length");
    static const uint8_t exp[] = { 0x1b, 0x40 };
    EXPECT_BYTES(buf, exp, 2, "init = ESC @");
}

static void test_status_request(void)
{
    uint8_t buf[4] = {0};
    int n = pt_encode_status_request(buf, sizeof buf);
    EXPECT(n == 3, "status request length");
    static const uint8_t exp[] = { 0x1b, 0x69, 0x53 };
    EXPECT_BYTES(buf, exp, 3, "status request = ESC i S");
}

static void test_switch_mode(void)
{
    uint8_t buf[8] = {0};
    int n = pt_encode_switch_mode(buf, sizeof buf, PT_CMD_MODE_RASTER);
    EXPECT(n == 4, "switch mode length");
    static const uint8_t raster[] = { 0x1b, 0x69, 0x61, 0x01 };
    EXPECT_BYTES(buf, raster, 4, "switch mode raster = ESC i a 0x01");

    n = pt_encode_switch_mode(buf, sizeof buf, PT_CMD_MODE_PT_TEMPLATE);
    EXPECT(n == 4, "switch mode length");
    static const uint8_t tmpl[] = { 0x1b, 0x69, 0x61, 0x03 };
    EXPECT_BYTES(buf, tmpl, 4, "switch mode template = ESC i a 0x03");
}

/* The SDM §2.2.3 test-page example sends:
 *   1B 69 7A 84 00 18 00 9C 02 00 00 00 00
 * Decoded: valid=0x84 (PI_RECOVER|PI_WIDTH), media_type=0, width=0x18=24,
 * length=0, raster_lines=0x0000029C=668, first page (n9=0), n10=0. */
static void test_print_info_sdm_example(void)
{
    pt_print_info_t info = {
        .media_type        = PT_MEDIA_NONE,
        .media_width_mm    = 24,
        .media_length_mm   = 0,
        .raster_lines      = 668,
        .is_first_page     = true,
        .valid_width       = true,
        .recover_always_on = true,
    };
    uint8_t buf[16] = {0};
    int n = pt_encode_print_info(buf, sizeof buf, &info);
    EXPECT(n == 13, "print info length");
    static const uint8_t exp[] = {
        0x1b, 0x69, 0x7a, 0x84, 0x00, 0x18, 0x00,
        0x9c, 0x02, 0x00, 0x00, 0x00, 0x00,
    };
    EXPECT_BYTES(buf, exp, 13, "SDM §2.2.3 print info bytes");
}

static void test_mode_settings(void)
{
    uint8_t buf[4] = {0};
    pt_mode_settings_t m = {0};
    int n = pt_encode_mode_settings(buf, sizeof buf, &m);
    EXPECT(n == 4, "mode settings length");
    static const uint8_t none[] = { 0x1b, 0x69, 0x4d, 0x00 };
    EXPECT_BYTES(buf, none, 4, "mode = nothing -> 0x00");

    m.auto_cut = true;
    pt_encode_mode_settings(buf, sizeof buf, &m);
    static const uint8_t cut[] = { 0x1b, 0x69, 0x4d, 0x40 };
    EXPECT_BYTES(buf, cut, 4, "mode = auto cut -> bit 6");

    m.mirror_print = true;
    pt_encode_mode_settings(buf, sizeof buf, &m);
    static const uint8_t both[] = { 0x1b, 0x69, 0x4d, 0xc0 };
    EXPECT_BYTES(buf, both, 4, "mode = cut + mirror -> bits 6,7");
}

static void test_advanced_settings(void)
{
    uint8_t buf[4] = {0};
    pt_advanced_settings_t a = { .no_chain_print = true };
    int n = pt_encode_advanced_settings(buf, sizeof buf, &a);
    EXPECT(n == 4, "advanced length");
    static const uint8_t exp[] = { 0x1b, 0x69, 0x4b, 0x08 };
    EXPECT_BYTES(buf, exp, 4, "advanced no-chain -> ESC i K 0x08");
}

static void test_margin(void)
{
    uint8_t buf[8] = {0};
    int n = pt_encode_margin(buf, sizeof buf, 15);  /* SDM example */
    EXPECT(n == 5, "margin length");
    static const uint8_t exp[] = { 0x1b, 0x69, 0x64, 0x0f, 0x00 };
    EXPECT_BYTES(buf, exp, 5, "margin 15 dots -> ESC i d 0x0f 0x00");

    pt_encode_margin(buf, sizeof buf, 0x1234);
    static const uint8_t le[] = { 0x1b, 0x69, 0x64, 0x34, 0x12 };
    EXPECT_BYTES(buf, le, 5, "margin little-endian");
}

static void test_compression(void)
{
    uint8_t buf[4] = {0};
    int n = pt_encode_compression(buf, sizeof buf, PT_COMPRESSION_TIFF);
    EXPECT(n == 2, "compression length");
    static const uint8_t tiff[] = { 0x4d, 0x02 };
    EXPECT_BYTES(buf, tiff, 2, "compression TIFF -> M 0x02");

    pt_encode_compression(buf, sizeof buf, PT_COMPRESSION_NONE);
    static const uint8_t none[] = { 0x4d, 0x00 };
    EXPECT_BYTES(buf, none, 2, "compression none -> M 0x00");
}

static void test_raster_row(void)
{
    uint8_t row[16];
    for (size_t i = 0; i < 16; i++) row[i] = (uint8_t)(0xa0 + i);
    uint8_t buf[32] = {0};
    int n = pt_encode_raster_row(buf, sizeof buf, row, 16);
    EXPECT(n == 19, "raster row length");
    EXPECT(buf[0] == 0x67 && buf[1] == 0x10 && buf[2] == 0x00,
           "raster row framing g 0x10 0x00");
    EXPECT_BYTES(buf + 3, row, 16, "raster row payload");
}

static void test_zero_row(void)
{
    uint8_t buf[2] = {0};
    int n = pt_encode_zero_row(buf, sizeof buf);
    EXPECT(n == 1, "zero row length");
    EXPECT(buf[0] == 0x5a, "zero row = 0x5A");
}

static void test_print_terminators(void)
{
    uint8_t buf[2] = {0};
    EXPECT(pt_encode_print_page(buf, sizeof buf) == 1, "page len");
    EXPECT(buf[0] == 0x0c, "page = FF (0x0C)");
    EXPECT(pt_encode_print_last(buf, sizeof buf) == 1, "last len");
    EXPECT(buf[0] == 0x1a, "last = Ctrl-Z (0x1A)");
}

static void test_tape_geometry(void)
{
    pt_tape_geometry_t g;
    EXPECT(pt_tape_geometry_tze(24, &g) == PT_OK, "24mm tape known");
    EXPECT(g.total_pins == 128 && g.print_pins == 128
        && g.left_margin_pins == 0 && g.right_margin_pins == 0,
           "24mm -> 0/128/0");

    EXPECT(pt_tape_geometry_tze(12, &g) == PT_OK, "12mm tape known");
    EXPECT(g.left_margin_pins == 29 && g.print_pins == 70
        && g.right_margin_pins == 29, "12mm -> 29/70/29");

    EXPECT(pt_tape_geometry_tze(7, &g) == PT_ERR_INVALID_ARG,
           "unknown width rejected");
}

static void test_buf_too_small(void)
{
    uint8_t b[1];
    EXPECT(pt_encode_init(b, 1) == PT_ERR_BUF_TOO_SMALL, "init wants 2");
    EXPECT(pt_encode_status_request(b, 2) == PT_ERR_BUF_TOO_SMALL, "status wants 3");
    EXPECT(pt_encode_switch_mode(b, 3, PT_CMD_MODE_RASTER)
           == PT_ERR_BUF_TOO_SMALL, "switch wants 4");
    pt_print_info_t info = {0};
    EXPECT(pt_encode_print_info(b, 12, &info) == PT_ERR_BUF_TOO_SMALL,
           "print info wants 13");
    EXPECT(pt_encode_margin(b, 4, 0) == PT_ERR_BUF_TOO_SMALL, "margin wants 5");
    EXPECT(pt_encode_compression(b, 1, PT_COMPRESSION_TIFF)
           == PT_ERR_BUF_TOO_SMALL, "compression wants 2");
}

int main(void)
{
    test_invalidate();
    test_init();
    test_status_request();
    test_switch_mode();
    test_print_info_sdm_example();
    test_mode_settings();
    test_advanced_settings();
    test_margin();
    test_compression();
    test_raster_row();
    test_zero_row();
    test_print_terminators();
    test_tape_geometry();
    test_buf_too_small();
    printf("encoder: all tests passed\n");
    return 0;
}
