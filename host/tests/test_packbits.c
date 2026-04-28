/* PackBits encoder + decoder tests. Reference: SDM §4 M (p. 35). */

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
        fprintf(stderr, "\n"); exit(1); \
    } \
} while (0)

/* SDM §4 M example, p. 35:
 *   uncompressed = 20×0x00, 2×0x22, 0x23, 0xBA, 0xBF, 0xA2, 0x22, 0x2B
 *   compressed   = ED 00 FF 22 05 23 BA BF A2 22 2B */
static void test_sdm_example(void)
{
    uint8_t in[28];
    memset(in, 0x00, 20);
    in[20] = 0x22; in[21] = 0x22;
    in[22] = 0x23; in[23] = 0xBA; in[24] = 0xBF;
    in[25] = 0xA2; in[26] = 0x22; in[27] = 0x2B;

    uint8_t out[64] = {0};
    int n = pt_packbits_encode(out, sizeof out, in, sizeof in);
    EXPECT(n == 11, "SDM example produces 11 bytes");
    static const uint8_t exp[] = {
        0xED, 0x00, 0xFF, 0x22, 0x05,
        0x23, 0xBA, 0xBF, 0xA2, 0x22, 0x2B
    };
    EXPECT_BYTES(out, exp, 11, "SDM §4 M example bytes");
}

static void test_all_zeros(void)
{
    uint8_t in[16] = {0};
    uint8_t out[32] = {0};
    int n = pt_packbits_encode(out, sizeof out, in, sizeof in);
    /* 16 zero bytes -> repeat code (1-16=-15=0xF1) + 0x00 = 2 bytes. */
    EXPECT(n == 2, "16 zeros -> 2 bytes");
    EXPECT(out[0] == 0xF1 && out[1] == 0x00, "zeros encode as F1 00");
}

static void test_all_distinct(void)
{
    uint8_t in[16];
    for (size_t i = 0; i < 16; i++) in[i] = (uint8_t)(0xA0 + i);
    uint8_t out[32] = {0};
    int n = pt_packbits_encode(out, sizeof out, in, sizeof in);
    /* 16 distinct bytes -> raw run, length byte 0x0F + 16 bytes. */
    EXPECT(n == 17, "16 distinct -> 17 bytes");
    EXPECT(out[0] == 0x0F, "leading length 0x0F");
    EXPECT_BYTES(out + 1, in, 16, "raw payload");
}

static void test_roundtrip(void)
{
    /* Random-ish input designed to exercise both repeat and raw paths. */
    uint8_t in[32] = {
        0x00, 0x00, 0x00, 0x00, 0xAA, 0xBB, 0xCC, 0xDD,
        0xEE, 0xFF, 0x11, 0x11, 0x22, 0x33, 0x44, 0x55,
        0x55, 0x55, 0x55, 0x55, 0xFF, 0xAA, 0x55, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x77, 0x88, 0x99,
    };
    uint8_t enc[64] = {0};
    int n = pt_packbits_encode(enc, sizeof enc, in, sizeof in);
    EXPECT(n > 0, "encode succeeds");

    uint8_t dec[64] = {0};
    int m = pt_packbits_decode(dec, sizeof dec, enc, (size_t)n);
    EXPECT(m == 32, "decode length matches");
    EXPECT_BYTES(dec, in, 32, "round-trip equality");
}

static void test_decode_bad_inputs(void)
{
    uint8_t out[16];
    /* Truncated raw chunk: claims 5 bytes but only 2 follow. */
    uint8_t bad_raw[] = { 0x04, 0x01, 0x02 };
    EXPECT(pt_packbits_decode(out, sizeof out, bad_raw, sizeof bad_raw)
           == PT_ERR_INVALID_ARG, "truncated raw chunk rejected");
    /* Repeat code with no following byte. */
    uint8_t bad_rep[] = { 0xFF };
    EXPECT(pt_packbits_decode(out, sizeof out, bad_rep, sizeof bad_rep)
           == PT_ERR_INVALID_ARG, "truncated repeat rejected");
}

int main(void)
{
    test_sdm_example();
    test_all_zeros();
    test_all_distinct();
    test_roundtrip();
    test_decode_bad_inputs();
    printf("packbits: all tests passed\n");
    return 0;
}
