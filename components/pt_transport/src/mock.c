/*
 * pt_transport_mock — virtual PT-P700 state machine backing the
 * pt_transport_t interface. See pt_transport_mock.h.
 */

#include "pt_transport_mock.h"

#include <string.h>

/* ---------------------------------------------------------------- rx ring */

static int rx_push(pt_transport_mock_t *m, const uint8_t *src, size_t n)
{
    if (m->rx_len + n > PT_MOCK_RX_CAP) return -1;
    for (size_t i = 0; i < n; i++) {
        m->rx_buf[m->rx_head] = src[i];
        m->rx_head = (m->rx_head + 1) % PT_MOCK_RX_CAP;
    }
    m->rx_len += n;
    return 0;
}

static size_t rx_pop(pt_transport_mock_t *m, uint8_t *dst, size_t cap)
{
    size_t n = (cap < m->rx_len) ? cap : m->rx_len;
    for (size_t i = 0; i < n; i++) {
        dst[i] = m->rx_buf[m->rx_tail];
        m->rx_tail = (m->rx_tail + 1) % PT_MOCK_RX_CAP;
    }
    m->rx_len -= n;
    return n;
}

/* --------------------------------------------------------- status emitter */

static void emit_status(pt_transport_mock_t *m,
                        pt_status_type_t status, pt_phase_type_t phase)
{
    uint8_t s[32] = {0};
    s[0]  = 0x80;          /* head mark */
    s[1]  = 0x20;          /* size */
    s[2]  = 'B';
    s[3]  = '0';
    s[4]  = (uint8_t)m->model;
    s[5]  = '0';
    s[8]  = m->error1;
    s[9]  = m->error2;
    s[10] = m->media_width_mm;
    s[11] = (uint8_t)m->media_type;
    s[18] = (m->error1 || m->error2)
              ? (uint8_t)PT_STATUS_ERROR_OCCURRED
              : (uint8_t)status;
    s[19] = (uint8_t)phase;
    s[22] = (uint8_t)m->notification;
    s[24] = m->tape_color_id;
    s[25] = m->text_color_id;
    (void)rx_push(m, s, 32);
}

/* ----------------------------------------------------------- byte parser */

/* Returns the number of bytes consumed, or 0 if the next command is
 * incomplete and we should wait for more. Returns -1 on a malformed
 * opcode (caller advances 1 byte and continues). */
static int parse_one(pt_transport_mock_t *m, const uint8_t *p, size_t n)
{
    uint8_t op = p[0];
    if (op == 0x00) return 1;          /* NULL invalidate filler */

    if (op == 0x1b) {                  /* ESC … */
        if (n < 2) return 0;
        uint8_t op2 = p[1];
        if (op2 == 0x40) return 2;     /* ESC @ init */
        if (op2 != 0x69) return -1;
        if (n < 3) return 0;
        switch (p[2]) {
        case 'S':                      /* ESC i S — status request */
            emit_status(m, PT_STATUS_REPLY, PT_PHASE_EDITING);
            return 3;
        case 'a':                      /* ESC i a {n} */
            if (n < 4) return 0;
            m->cmd_mode = (pt_command_mode_t)p[3];
            return 4;
        case 'z':                      /* ESC i z 10 bytes */
            return n < 13 ? 0 : 13;
        case 'M':                      /* ESC i M {n} */
        case 'K':                      /* ESC i K {n} */
            return n < 4 ? 0 : 4;
        case 'd':                      /* ESC i d {n1}{n2} */
            return n < 5 ? 0 : 5;
        default: return -1;
        }
    }

    if (op == 0x4d) {                  /* M {n} compression */
        if (n < 2) return 0;
        m->compression = (pt_compression_t)p[1];
        return 2;
    }

    if (op == 0x47) {                  /* G {lo}{hi}{data} raster row */
        if (n < 3) return 0;
        size_t pl = (size_t)p[1] | ((size_t)p[2] << 8);
        if (n < 3 + pl) return 0;
        m->rows_received++;
        return (int)(3 + pl);
    }

    if (op == 0x5a) {                  /* Z zero row */
        m->rows_received++;
        return 1;
    }

    if (op == 0x0c || op == 0x1a) {    /* page / page+feed */
        m->pages_completed++;
        emit_status(m, PT_STATUS_PHASE_CHANGE,   PT_PHASE_PRINTING);
        emit_status(m, PT_STATUS_PRINTING_DONE,  PT_PHASE_PRINTING);
        emit_status(m, PT_STATUS_PHASE_CHANGE,   PT_PHASE_EDITING);
        return 1;
    }

    return -1;
}

static void parse_pending(pt_transport_mock_t *m)
{
    while (m->tx_parsed < m->tx_len) {
        const uint8_t *p = m->tx_buf + m->tx_parsed;
        size_t n = m->tx_len - m->tx_parsed;
        int consumed = parse_one(m, p, n);
        if (consumed == 0) return;     /* wait for more bytes */
        if (consumed < 0) consumed = 1; /* skip malformed byte, keep going */
        m->tx_parsed += (size_t)consumed;
    }
}

/* --------------------------------------------------------- transport API */

static int mock_send(void *ctx, const uint8_t *data, size_t len)
{
    pt_transport_mock_t *m = ctx;
    if (m->tx_len + len > PT_MOCK_TX_CAP) return -1;
    memcpy(m->tx_buf + m->tx_len, data, len);
    m->tx_len += len;
    parse_pending(m);
    return (int)len;
}

static int mock_recv(void *ctx, uint8_t *out, size_t cap,
                     size_t *out_len, uint32_t timeout_ms)
{
    (void)timeout_ms;  /* mock has no async I/O */
    pt_transport_mock_t *m = ctx;
    *out_len = rx_pop(m, out, cap);
    return 0;
}

/* ----------------------------------------------------------------- API */

pt_transport_t pt_transport_mock_init(pt_transport_mock_t *m)
{
    memset(m, 0, sizeof *m);
    m->model           = PT_MODEL_P700;
    m->media_width_mm  = 24;
    m->media_type      = PT_MEDIA_LAMINATED;
    m->tape_color_id   = 0x01;   /* white */
    m->text_color_id   = 0x08;   /* black */
    m->cmd_mode        = PT_CMD_MODE_ESCP;
    m->compression     = PT_COMPRESSION_NONE;
    return (pt_transport_t){
        .send = mock_send,
        .recv = mock_recv,
        .ctx  = m,
    };
}

void pt_transport_mock_reset(pt_transport_mock_t *m)
{
    m->tx_len = 0; m->tx_parsed = 0;
    m->rx_head = m->rx_tail = m->rx_len = 0;
    m->cmd_mode = PT_CMD_MODE_ESCP;
    m->compression = PT_COMPRESSION_NONE;
    m->rows_received = 0;
    m->pages_completed = 0;
}

void pt_transport_mock_set_media(pt_transport_mock_t *m,
                                 uint8_t width_mm,
                                 pt_media_type_t type,
                                 uint8_t tape_color_id,
                                 uint8_t text_color_id)
{
    m->media_width_mm = width_mm;
    m->media_type     = type;
    m->tape_color_id  = tape_color_id;
    m->text_color_id  = text_color_id;
}

void pt_transport_mock_set_error1(pt_transport_mock_t *m, uint8_t f) { m->error1 = f; }
void pt_transport_mock_set_error2(pt_transport_mock_t *m, uint8_t f) { m->error2 = f; }

size_t          pt_transport_mock_tx_size(const pt_transport_mock_t *m) { return m->tx_len; }
const uint8_t  *pt_transport_mock_tx_data(const pt_transport_mock_t *m) { return m->tx_buf; }
