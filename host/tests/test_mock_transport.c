/* Drives the mock transport through several SDM scenarios. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_protocol.h"
#include "pt_transport.h"
#include "pt_transport_mock.h"

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); exit(1); } \
} while (0)

/* Helper: send a status request, drain the 32-byte reply, decode it. */
static void status_request(const pt_transport_t *t, pt_status_t *out)
{
    uint8_t cmd[8];
    int n = pt_encode_status_request(cmd, sizeof cmd);
    EXPECT(pt_transport_send(t, cmd, (size_t)n) == n, "send status req");

    uint8_t resp[64];
    size_t  resp_len = 0;
    EXPECT(pt_transport_recv(t, resp, sizeof resp, &resp_len, 100) == 0,
           "recv status reply");
    EXPECT(resp_len == 32, "status reply is 32 bytes");
    EXPECT(pt_status_decode(resp, resp_len, out) == PT_OK, "decode status");
}

static void test_default_idle(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);
    pt_status_t st;
    status_request(&t, &st);
    EXPECT(st.model == PT_MODEL_P700, "default model = P-700");
    EXPECT(st.media_width_mm == 24, "default tape = 24 mm");
    EXPECT(st.media_type == PT_MEDIA_LAMINATED, "default tape = laminated");
    EXPECT(st.tape_color_id == 0x01, "default tape colour = white");
    EXPECT(st.text_color_id == 0x08, "default text colour = black");
    EXPECT(st.status_type == PT_STATUS_REPLY, "status = reply");
    EXPECT(st.error1 == 0 && st.error2 == 0, "no errors");
}

static void test_error_injection(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    pt_transport_mock_set_error1(&mock, PT_ERR1_NO_MEDIA);
    pt_transport_mock_set_error2(&mock, PT_ERR2_COVER_OPEN);

    pt_status_t st;
    status_request(&t, &st);
    EXPECT(st.error1 & PT_ERR1_NO_MEDIA, "no-media reported");
    EXPECT(st.error2 & PT_ERR2_COVER_OPEN, "cover-open reported");
    EXPECT(st.status_type == PT_STATUS_ERROR_OCCURRED,
           "status_type forced to ERROR");
}

static void test_media_change(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);
    pt_transport_mock_set_media(&mock, 12, PT_MEDIA_NON_LAMINATED, 0x06, 0x04);

    pt_status_t st;
    status_request(&t, &st);
    EXPECT(st.media_width_mm == 12, "12 mm tape reported");
    EXPECT(st.media_type == PT_MEDIA_NON_LAMINATED, "non-laminated");
    EXPECT(st.tape_color_id == 0x06, "tape yellow");
    EXPECT(st.text_color_id == 0x04, "text red");
}

/* Full-job flow per SDM §5.1: send init burst + raster + Ctrl-Z, expect
 * the mock to enqueue Phase=Printing → Done → Phase=Editing transitions. */
static void test_full_job_flow(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    /* Drain the per-status reply we'd otherwise get from the initial
     * status check by NOT issuing one — this test focuses on the print-
     * complete burst. */

    uint8_t buf[64];
    int n;

    n = pt_encode_invalidate(buf, sizeof buf);
    EXPECT(pt_transport_send(&t, buf, n) == n, "invalidate");

    n = pt_encode_init(buf, sizeof buf);
    EXPECT(pt_transport_send(&t, buf, n) == n, "init");

    n = pt_encode_switch_mode(buf, sizeof buf, PT_CMD_MODE_RASTER);
    EXPECT(pt_transport_send(&t, buf, n) == n, "switch raster");

    pt_print_info_t info = {
        .media_type = PT_MEDIA_LAMINATED, .media_width_mm = 24,
        .raster_lines = 2, .is_first_page = true,
        .valid_kind = true, .valid_width = true,
    };
    n = pt_encode_print_info(buf, sizeof buf, &info);
    EXPECT(pt_transport_send(&t, buf, n) == n, "print info");

    /* Two zero raster rows (simplest possible "page"). */
    n = pt_encode_zero_row(buf, sizeof buf);
    EXPECT(pt_transport_send(&t, buf, n) == n, "row 1");
    EXPECT(pt_transport_send(&t, buf, n) == n, "row 2");

    n = pt_encode_print_last(buf, sizeof buf);
    EXPECT(pt_transport_send(&t, buf, n) == n, "Ctrl-Z");

    /* The Ctrl-Z should have triggered three 32-byte status messages. */
    uint8_t rx[256];
    size_t rx_len = 0;
    EXPECT(pt_transport_recv(&t, rx, sizeof rx, &rx_len, 100) == 0, "recv");
    EXPECT(rx_len == 96, "3 x 32-byte status messages");

    pt_status_t s;
    EXPECT(pt_status_decode(rx +  0, 32, &s) == PT_OK, "decode 1");
    EXPECT(s.status_type == PT_STATUS_PHASE_CHANGE && s.phase_type == PT_PHASE_PRINTING,
           "msg 1: phase change → printing");
    EXPECT(pt_status_decode(rx + 32, 32, &s) == PT_OK, "decode 2");
    EXPECT(s.status_type == PT_STATUS_PRINTING_DONE, "msg 2: printing done");
    EXPECT(pt_status_decode(rx + 64, 32, &s) == PT_OK, "decode 3");
    EXPECT(s.status_type == PT_STATUS_PHASE_CHANGE && s.phase_type == PT_PHASE_EDITING,
           "msg 3: phase change → editing");

    EXPECT(mock.rows_received == 2, "two raster rows seen");
    EXPECT(mock.pages_completed == 1, "one page completed");
    EXPECT(mock.cmd_mode == PT_CMD_MODE_RASTER, "cmd mode raster");
}

/* The parser must tolerate a command being split across multiple sends. */
static void test_partial_send(void)
{
    pt_transport_mock_t mock;
    pt_transport_t t = pt_transport_mock_init(&mock);

    /* ESC i S = three bytes. Send them one at a time. */
    EXPECT(pt_transport_send(&t, (const uint8_t[]){0x1b}, 1) == 1, "byte 1");
    EXPECT(pt_transport_send(&t, (const uint8_t[]){0x69}, 1) == 1, "byte 2");

    /* No reply yet — command incomplete. */
    uint8_t resp[64];
    size_t  resp_len = 0;
    pt_transport_recv(&t, resp, sizeof resp, &resp_len, 0);
    EXPECT(resp_len == 0, "no reply for incomplete command");

    EXPECT(pt_transport_send(&t, (const uint8_t[]){0x53}, 1) == 1, "byte 3");
    pt_transport_recv(&t, resp, sizeof resp, &resp_len, 0);
    EXPECT(resp_len == 32, "reply arrives once command is complete");
}

int main(void)
{
    test_default_idle();
    test_error_injection();
    test_media_change();
    test_full_job_flow();
    test_partial_send();
    printf("mock transport: all tests passed\n");
    return 0;
}
