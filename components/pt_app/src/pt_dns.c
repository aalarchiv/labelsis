#include "pt_dns.h"

#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pt_dns";

/* SoftAP IP from esp_netif defaults. If pt_app ever changes the AP
 * subnet, update here too. */
#define HIJACK_IP_BYTE0 192
#define HIJACK_IP_BYTE1 168
#define HIJACK_IP_BYTE2 4
#define HIJACK_IP_BYTE3 1

#define DNS_PORT       53
#define DNS_BUF_BYTES  512  /* RFC 1035 max UDP payload */
#define DNS_HDR_BYTES  12

/* DNS header offsets (network byte order, but we only set bytes). */
#define OFF_FLAGS_HI 2
#define OFF_FLAGS_LO 3
#define OFF_QDCOUNT  4
#define OFF_ANCOUNT  6
#define OFF_NSCOUNT  8
#define OFF_ARCOUNT  10

#define QTYPE_A    1
#define CLASS_IN   1

static TaskHandle_t s_task;
static int          s_sock = -1;
static volatile bool s_stop;

/* Walk a DNS-encoded name starting at buf[off]. Returns the byte
 * after the trailing zero label, or -1 on malformed input. Compressed
 * pointers are not expected in queries; we treat them as malformed. */
static int skip_qname(const uint8_t *buf, int len, int off)
{
    while (off < len) {
        uint8_t l = buf[off];
        if (l == 0) return off + 1;
        if ((l & 0xC0) != 0) return -1;  /* compression -> reject */
        off += 1 + l;
    }
    return -1;
}

static void handle_query(uint8_t *buf, int len,
                         struct sockaddr_in *from, socklen_t fromlen)
{
    if (len < DNS_HDR_BYTES) return;

    /* Only respond to standard queries (opcode 0) with QDCOUNT >= 1. */
    uint16_t qd = ((uint16_t)buf[OFF_QDCOUNT] << 8) | buf[OFF_QDCOUNT + 1];
    if (qd < 1) return;
    if ((buf[OFF_FLAGS_HI] & 0x80) != 0) return;  /* already a response */

    int qend = skip_qname(buf, len, DNS_HDR_BYTES);
    if (qend < 0 || qend + 4 > len) return;

    uint16_t qtype  = ((uint16_t)buf[qend]     << 8) | buf[qend + 1];
    /* uint16_t qclass would be at qend+2; we ignore class. */
    int after_q = qend + 4;

    /* Build reply in place. Flags: QR=1, AA=1, RA=1, RCODE=0. */
    buf[OFF_FLAGS_HI] = 0x84;
    buf[OFF_FLAGS_LO] = 0x80;
    /* QDCOUNT stays as-is (echo question). NSCOUNT/ARCOUNT zeroed. */
    buf[OFF_NSCOUNT]     = 0; buf[OFF_NSCOUNT + 1] = 0;
    buf[OFF_ARCOUNT]     = 0; buf[OFF_ARCOUNT + 1] = 0;

    int reply_len;
    if (qtype == QTYPE_A) {
        if (after_q + 16 > DNS_BUF_BYTES) return;
        buf[OFF_ANCOUNT] = 0; buf[OFF_ANCOUNT + 1] = 1;
        int o = after_q;
        /* Name pointer to question (offset 12). */
        buf[o++] = 0xC0; buf[o++] = DNS_HDR_BYTES;
        /* TYPE A, CLASS IN. */
        buf[o++] = 0; buf[o++] = QTYPE_A;
        buf[o++] = 0; buf[o++] = CLASS_IN;
        /* TTL 60 s. */
        buf[o++] = 0; buf[o++] = 0; buf[o++] = 0; buf[o++] = 60;
        /* RDLENGTH 4, RDATA = AP IP. */
        buf[o++] = 0; buf[o++] = 4;
        buf[o++] = HIJACK_IP_BYTE0; buf[o++] = HIJACK_IP_BYTE1;
        buf[o++] = HIJACK_IP_BYTE2; buf[o++] = HIJACK_IP_BYTE3;
        reply_len = o;
    } else {
        /* For AAAA / TXT / etc., return an empty answer rather than
         * NXDOMAIN. iOS will then proceed to the A query and trigger
         * the captive portal cleanly. */
        buf[OFF_ANCOUNT] = 0; buf[OFF_ANCOUNT + 1] = 0;
        reply_len = after_q;
    }

    sendto(s_sock, buf, reply_len, 0, (struct sockaddr *)from, fromlen);
}

static void dns_task(void *arg)
{
    uint8_t buf[DNS_BUF_BYTES];
    while (!s_stop) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        int n = recvfrom(s_sock, buf, sizeof buf, 0,
                         (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            if (s_stop) break;
            if (errno == EAGAIN || errno == EINTR) continue;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        handle_query(buf, n, &from, fromlen);
    }
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t pt_dns_hijack_start(void)
{
    if (s_task) return ESP_OK;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        return ESP_FAIL;
    }

    /* Block recvfrom but let stop() interrupt within ~1 s. */
    struct timeval to = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&addr, sizeof addr) < 0) {
        ESP_LOGE(TAG, "bind :53 failed: errno %d", errno);
        close(s_sock); s_sock = -1;
        return ESP_FAIL;
    }

    s_stop = false;
    BaseType_t r = xTaskCreate(dns_task, "pt_dns", 4096, NULL, 4, &s_task);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed");
        close(s_sock); s_sock = -1;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "captive-portal DNS up, hijacking to %d.%d.%d.%d",
             HIJACK_IP_BYTE0, HIJACK_IP_BYTE1,
             HIJACK_IP_BYTE2, HIJACK_IP_BYTE3);
    return ESP_OK;
}

void pt_dns_hijack_stop(void)
{
    if (!s_task) return;
    s_stop = true;
    /* Task drains its own socket close on the next 1 s timeout. */
}
