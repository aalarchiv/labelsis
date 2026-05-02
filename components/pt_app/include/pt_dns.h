#ifndef PT_DNS_H
#define PT_DNS_H

/*
 * pt_dns -- minimal captive-portal DNS server.
 *
 * In AP-onboarding mode, listens on UDP/53 and answers every A query
 * with the SoftAP's own address (192.168.4.1 in our config). Phones
 * use this as the "captive portal" trigger: their connectivity probes
 * (Apple, Google, Microsoft) resolve to us, get HTML back, and the OS
 * pops the "Sign in to network" sheet straight onto the setup page --
 * no manual URL entry needed.
 *
 * Only meant to run while the device is in pure AP-onboarding mode;
 * stop the task before bringing up STA so we don't poison real DNS.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t pt_dns_hijack_start(void);
void      pt_dns_hijack_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* PT_DNS_H */
