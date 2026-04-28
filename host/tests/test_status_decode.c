/* 32-byte status struct decoder tests against the SDM v1.11 layout. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pt_protocol.h"

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); exit(1); } \
} while (0)

/* Helper: build a "happy path" 32-byte status response for a P-700 with
 * a 24 mm laminated tape, no errors, idle (reply-to-status). */
static void make_idle_p700(uint8_t buf[32])
{
    memset(buf, 0, 32);
    buf[0]  = 0x80;            /* print head mark            */
    buf[1]  = 0x20;            /* size (fixed 32)            */
    buf[2]  = 0x42;            /* 'B'                        */
    buf[3]  = 0x30;            /* '0'                        */
    buf[4]  = 0x67;            /* model 'g' = PT-P700        */
    buf[5]  = 0x30;            /* country '0'                */
    buf[10] = 24;              /* media width = 24 mm        */
    buf[11] = 0x01;            /* media type = laminated     */
    buf[18] = 0x00;            /* status type = reply        */
    buf[19] = 0x00;            /* phase = editing            */
    buf[24] = 0x01;            /* tape colour = white        */
    buf[25] = 0x08;            /* text colour = black        */
}

static void test_idle_decode(void)
{
    uint8_t raw[32];
    make_idle_p700(raw);
    pt_status_t st = {0};
    EXPECT(pt_status_decode(raw, 32, &st) == PT_OK, "decode idle");
    EXPECT(st.model == PT_MODEL_P700, "model");
    EXPECT(st.error1 == 0, "no error1");
    EXPECT(st.error2 == 0, "no error2");
    EXPECT(st.media_width_mm == 24, "width");
    EXPECT(st.media_type == PT_MEDIA_LAMINATED, "media type");
    EXPECT(st.status_type == PT_STATUS_REPLY, "status type");
    EXPECT(st.phase_type == PT_PHASE_EDITING, "phase");
    EXPECT(st.notification == PT_NOTIF_NONE, "no notification");
    EXPECT(st.tape_color_id == 0x01, "tape colour");
    EXPECT(st.text_color_id == 0x08, "text colour");
}

static void test_error_flags(void)
{
    uint8_t raw[32]; make_idle_p700(raw);
    raw[8]  = PT_ERR1_NO_MEDIA | PT_ERR1_CUTTER_JAM;
    raw[9]  = PT_ERR2_COVER_OPEN;
    raw[18] = PT_STATUS_ERROR_OCCURRED;

    pt_status_t st;
    EXPECT(pt_status_decode(raw, 32, &st) == PT_OK, "decode w/ errors");
    EXPECT((st.error1 & PT_ERR1_NO_MEDIA) != 0, "no-media bit");
    EXPECT((st.error1 & PT_ERR1_CUTTER_JAM) != 0, "cutter-jam bit");
    EXPECT((st.error1 & PT_ERR1_WEAK_BATTERY) == 0, "no weak-battery");
    EXPECT((st.error2 & PT_ERR2_COVER_OPEN) != 0, "cover-open bit");
    EXPECT(st.status_type == PT_STATUS_ERROR_OCCURRED, "status = error");
}

static void test_notification_phase(void)
{
    uint8_t raw[32]; make_idle_p700(raw);
    raw[18] = PT_STATUS_NOTIFICATION;
    raw[22] = PT_NOTIF_COVER_OPEN;

    pt_status_t st;
    EXPECT(pt_status_decode(raw, 32, &st) == PT_OK, "decode notification");
    EXPECT(st.status_type == PT_STATUS_NOTIFICATION, "notif status");
    EXPECT(st.notification == PT_NOTIF_COVER_OPEN, "cover-open notif");
}

static void test_phase_change_printing(void)
{
    uint8_t raw[32]; make_idle_p700(raw);
    raw[18] = PT_STATUS_PHASE_CHANGE;
    raw[19] = PT_PHASE_PRINTING;
    raw[20] = 0x12; raw[21] = 0x34;

    pt_status_t st;
    EXPECT(pt_status_decode(raw, 32, &st) == PT_OK, "decode phase change");
    EXPECT(st.status_type == PT_STATUS_PHASE_CHANGE, "phase change");
    EXPECT(st.phase_type == PT_PHASE_PRINTING, "printing phase");
    EXPECT(st.phase_number == 0x1234, "phase number BE-packed");
}

static void test_hardware_settings(void)
{
    uint8_t raw[32]; make_idle_p700(raw);
    raw[26] = 0xde; raw[27] = 0xad; raw[28] = 0xbe; raw[29] = 0xef;
    pt_status_t st;
    EXPECT(pt_status_decode(raw, 32, &st) == PT_OK, "decode hw settings");
    EXPECT(st.hardware_settings == 0xdeadbeef, "hw settings BE-packed");
}

static void test_bad_inputs(void)
{
    uint8_t raw[32] = {0};
    pt_status_t st;

    EXPECT(pt_status_decode(NULL, 32, &st) == PT_ERR_INVALID_ARG, "null buf");
    EXPECT(pt_status_decode(raw, 32, NULL) == PT_ERR_INVALID_ARG, "null out");
    EXPECT(pt_status_decode(raw, 31, &st) == PT_ERR_BAD_LEN, "short");
    EXPECT(pt_status_decode(raw, 33, &st) == PT_ERR_BAD_LEN, "long");

    raw[0] = 0x00;  /* missing magic */
    EXPECT(pt_status_decode(raw, 32, &st) == PT_ERR_BAD_MAGIC, "bad magic");
}

int main(void)
{
    test_idle_decode();
    test_error_flags();
    test_notification_phase();
    test_phase_change_printing();
    test_hardware_settings();
    test_bad_inputs();
    printf("status decode: all tests passed\n");
    return 0;
}
