/*
 * rcd_network.h -- The camera's address, and the file that holds it
 *
 * One interface: the camera's. If a wifi device is present it is that one,
 * otherwise it is the wired port -- the same question `S40network` asks, of
 * the same environment variable, so the boot script and this agree by
 * construction rather than by two guesses. A camera has one address worth
 * configuring and one worth showing, and a section per interface would only
 * raise the question of which one the console is talking about.
 *
 * The store is `/etc/network/interfaces.d/<iface>`, and the rule for writing
 * it is that only the five directives below are ours. The shipped stanza's
 * second line is
 *
 *     hwaddress ether $(fw_printenv -n ethaddr || echo 00:00:23:34:45:66)
 *
 * -- a shell substitution the interface's own MAC comes from, evaluated at
 * ifup. Rewriting the stanza as "iface + address + netmask + gateway" loses
 * it, and the camera comes back on a different MAC and a different lease. So
 * every other line is carried through untouched, including the ones a future
 * image adds.
 *
 * busybox's ifupdown stores any option it does not recognise rather than
 * refusing it (networking/ifupdown.c, read_interfaces), so the static
 * addresses stay in the stanza while DHCP is on and are still there when it
 * is turned off. Switching between the two is a change of one word.
 */

#ifndef RCD_NETWORK_H
#define RCD_NETWORK_H

#include "rcd_schema.h"

/*
 * Long enough for the interface to come down, DHCP to hand out a lease, ARP
 * caches to catch up and a browser to reload; short enough that nobody waits
 * it out by accident. This is the window the whole guard was built for.
 */
#define RCD_GUARD_NET_SEC 90

extern const rcd_provider_t rcd_provider_net_dhcp;
extern const rcd_provider_t rcd_provider_net_address;
extern const rcd_provider_t rcd_provider_net_netmask;
extern const rcd_provider_t rcd_provider_net_gateway;
extern const rcd_provider_t rcd_provider_net_dns;

/* Which interface the section describes. Resolved once, then remembered. */
const char *rcd_net_iface(void);

#endif /* RCD_NETWORK_H */
