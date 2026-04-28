/* pt_session tests — drive print jobs through the mock transport and
 * verify the byte stream + completion behaviour. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_protocol.h"
#include "pt_session.h"
#include "pt_transport_mock.h"

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); exit(1); } \
} while (0)

static void test_query_status(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    pt_status_t st;
    pt_err_t err = pt_session_query_status(&t, &st, NULL);
    EXPECT(err == PT_OK, "query status");
    EXPECT(st.media_width_mm == 24, "default 24mm reported");
    EXPECT(st.status_type == PT_STATUS_REPLY, "reply status");
}

static void test_print_zero_rows_job(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    /* Three all-zero rows — drives the zero-row shortcut path (0x5A). */
    uint8_t rows[3 * 16] = {0};

    pt_session_options_t opts;
    pt_session_options_default(&opts);
    opts.print_timeout_ms = 1000;  /* mock replies instantly */

    pt_err_t err = pt_session_print_raster(&t, rows, 3, 24, &opts);
    EXPECT(err == PT_OK, "print succeeds");
    EXPECT(mock.rows_received == 3, "3 raster rows seen by mock");
    EXPECT(mock.pages_completed == 1, "page completed");
    EXPECT(mock.cmd_mode == PT_CMD_MODE_RASTER, "raster mode set");
    EXPECT(mock.compression == PT_COMPRESSION_TIFF, "TIFF compression set");
}

static void test_print_real_data_job(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    /* 5 rows of varied data — exercises both packbits and zero-row paths. */
    uint8_t rows[5 * 16];
    memset(rows, 0, sizeof rows);
    /* Row 0: all zeros. Row 1: a single bit. Row 2: alternating. Row 3: all
     * 0xFF. Row 4: a run of repeats + a raw chunk. */
    rows[1 * 16 + 0] = 0x80;
    for (size_t i = 0; i < 16; i++) rows[2 * 16 + i] = (i & 1) ? 0xAA : 0x55;
    memset(rows + 3 * 16, 0xFF, 16);
    memcpy(rows + 4 * 16, (uint8_t[]){
        0x00, 0x00, 0x00, 0x00, 0x00, 0x22, 0x22, 0x23,
        0xBA, 0xBF, 0xA2, 0x22, 0x2B, 0x55, 0x55, 0x55,
    }, 16);

    pt_session_options_t opts;
    pt_session_options_default(&opts);
    opts.print_timeout_ms = 1000;

    pt_err_t err = pt_session_print_raster(&t, rows, 5, 24, &opts);
    EXPECT(err == PT_OK, "print succeeds");
    EXPECT(mock.rows_received == 5, "5 rows received");
    EXPECT(mock.pages_completed == 1, "1 page completed");
}

static void test_no_media_aborts(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);
    pt_transport_mock_set_error1(&mock, PT_ERR1_NO_MEDIA);

    uint8_t rows[16] = {0};
    pt_session_options_t opts;
    pt_session_options_default(&opts);
    opts.print_timeout_ms = 500;

    pt_err_t err = pt_session_print_raster(&t, rows, 1, 0, &opts);
    EXPECT(err == PT_ERR_NO_MEDIA, "no-media surfaced");
    EXPECT(mock.pages_completed == 0, "no page completed");
}

static void test_cover_open_aborts(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);
    pt_transport_mock_set_error2(&mock, PT_ERR2_COVER_OPEN);

    uint8_t rows[16] = {0};
    pt_err_t err = pt_session_print_raster(&t, rows, 1, 24, NULL);
    EXPECT(err == PT_ERR_COVER_OPEN, "cover-open surfaced");
}

static void test_media_mismatch(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);
    /* Mock has 24mm tape; ask for 12mm — should mismatch. */

    uint8_t rows[16] = {0};
    pt_err_t err = pt_session_print_raster(&t, rows, 1, 12, NULL);
    EXPECT(err == PT_ERR_MEDIA_MISMATCH, "media mismatch surfaced");
}

/* The byte stream the mock observed should follow the SDM-canonical
 * structure: invalidate + init + switch raster + ESC i z + ESC i M
 * + ESC i K + ESC i d + M + raster + Ctrl-Z. */
static void test_byte_stream_structure(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    uint8_t rows[16] = {0};
    pt_err_t err = pt_session_print_raster(&t, rows, 1, 24, NULL);
    EXPECT(err == PT_OK, "print succeeds");

    const uint8_t *tx = pt_transport_mock_tx_data(&mock);
    size_t         n  = pt_transport_mock_tx_size(&mock);
    EXPECT(n >= 100 + 2 + 4 + 3 + 13 + 4 + 4 + 5 + 2 + 1 + 1, "tx ≥ minimum");

    /* 100x 0x00 */
    for (size_t i = 0; i < 100; i++)
        EXPECT(tx[i] == 0x00, "invalidate prefix");
    EXPECT(tx[100] == 0x1b && tx[101] == 0x40, "ESC @");
    EXPECT(tx[102] == 0x1b && tx[103] == 0x69 && tx[104] == 0x61
        && tx[105] == 0x01, "switch raster");
    /* Then ESC i S (3 bytes), but reply is async — let's just verify it
     * appears next. */
    EXPECT(tx[106] == 0x1b && tx[107] == 0x69 && tx[108] == 0x53,
           "status request");
    /* The last byte must be Ctrl-Z. */
    EXPECT(tx[n - 1] == 0x1a, "trailing Ctrl-Z");
}

int main(void)
{
    test_query_status();
    test_print_zero_rows_job();
    test_print_real_data_job();
    test_no_media_aborts();
    test_cover_open_aborts();
    test_media_mismatch();
    test_byte_stream_structure();
    printf("session: all tests passed\n");
    return 0;
}
