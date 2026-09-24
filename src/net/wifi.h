/* Bringing the radio up, and saying so when it does not come.
 *
 * Connecting a PSP to an access point is five firmware calls and then a wait,
 * and the wait is the part that matters: sceNetApctlConnect returns success
 * long before there is an IP address, and every state between "scanning" and
 * "got IP" is one the connection can sit in forever if the AP is not there,
 * the key is wrong, or the stored configuration is empty. So this polls with
 * a bound, and turns whatever state it gave up in into a sentence rather than
 * a number (PROMPT.md section 47).
 *
 * WHICH ACCESS POINT: the index of one of the connection profiles the user
 * set up in the PSP's own network settings. There is no scanning and no key
 * entry here -- the console already has a perfectly good screen for that, and
 * PROMPT.md's MVP is a client for a network the operator already joined.
 *
 * MEMORY ORDER IS LOAD-BEARING: sceNetInit claims a pool of its own, and
 * docs/RESEARCH.md section 6 records that figure as roughly half a megabyte
 * with the note that a large claim made before it can leave the radio
 * nothing. Design section 3.3 says the same from the other side. The caller
 * decides the order; this file only says that it matters.
 *
 * Only compiled for the console. */
#ifndef NET_WIFI_H
#define NET_WIFI_H

/* Loads the net modules, starts the stack, and connects to the stored
 * configuration at `config_index` (a slot in the PSP's own network settings,
 * counting from 1), waiting up to `timeout_ms` for an address. 0 means "the
 * first slot that holds a profile" -- slots are not packed, so slot 1 can be
 * empty while slot 2 is the only one there.
 *
 * Returns 0 once there is an IP address. On failure returns non-zero and
 * leaves the reason in wifi_error(). Gives up early -- and says so -- if the
 * user presses HOME while it is waiting, because a connect that cannot
 * happen must not be the reason the console stops answering. */
int wifi_connect(int config_index, unsigned timeout_ms);

/* The address the AP gave us, or "" if there is none. Worth putting on the
 * screen: "connected" with no address is the failure that looks like
 * success. */
const char *wifi_ip(void);

/* The gateway and primary DNS server the connection was given, or "" if
 * there is no connection. Shown next to the address because a name that
 * will not resolve or a server that will not answer is most often one of
 * these two being wrong for the network the PSP is actually on. */
const char *wifi_gateway(void);
const char *wifi_dns(void);

/* The profile the last call used, as "slot name (SSID)", or "" if it never
 * got as far as choosing one. */
const char *wifi_profile(void);

/* The reason for the last failure, or "". Valid until the next call here. */
const char *wifi_error(void);

/* Takes the radio back down. Safe when wifi_connect failed or was never
 * called, and safe to call twice -- design section 3.4's teardown runs after
 * failures too. Does NOT unload the modules; see net/net_modules.h. */
void wifi_shutdown(void);

#endif /* NET_WIFI_H */
