/* libusb transport smoke test.
 *
 * Hardware-conditional: if no PT-* device is plugged in, exits 77 which
 * CTest treats as SKIP. With a printer connected: opens it, sends
 * ESC i S via pt_session, decodes the 32-byte status, prints media info
 * and exits 0. */

#include <stdio.h>
#include <stdlib.h>

#include "pt_protocol.h"
#include "pt_session.h"
#include "pt_transport.h"
#include "pt_transport_libusb.h"

#define EXIT_SKIP 77

int main(void)
{
    pt_transport_libusb_t *u = pt_transport_libusb_open();
    if (!u) {
        fprintf(stderr, "no PT-* device — skipping libusb smoke test\n");
        return EXIT_SKIP;
    }

    pt_transport_t t = pt_transport_libusb_transport(u);

    pt_status_t st;
    pt_err_t err = pt_session_query_status(&t, &st, NULL);
    if (err != PT_OK) {
        fprintf(stderr, "pt_session_query_status failed: %d\n", err);
        pt_transport_libusb_close(u);
        return 1;
    }

    printf("PT-* connected via libusb:\n");
    printf("  model:        0x%02x\n", st.model);
    printf("  media width:  %u mm\n",  st.media_width_mm);
    printf("  media type:   0x%02x\n", st.media_type);
    printf("  tape colour:  0x%02x\n", st.tape_color_id);
    printf("  text colour:  0x%02x\n", st.text_color_id);
    printf("  error1:       0x%02x\n", st.error1);
    printf("  error2:       0x%02x\n", st.error2);
    printf("  status type:  0x%02x\n", st.status_type);

    pt_transport_libusb_close(u);
    return 0;
}
