/* Shared public IPv4 policy for image fetches; no DNS or socket operations. */
#ifndef SB_PUBLIC_ADDRESS_H
#define SB_PUBLIC_ADDRESS_H
#include <stdint.h>

/** Reject local, private, reserved, documentation and multicast IPv4 ranges. */
static int sb_public_ipv4(const unsigned char *p)
{
	if (!p[0] || p[0] == 10 || p[0] == 127 || p[0] >= 224)
		return 0;
	if (p[0] == 100 && p[1] >= 64 && p[1] <= 127)
		return 0;
	if (p[0] == 169 && p[1] == 254)
		return 0;
	if (p[0] == 172 && p[1] >= 16 && p[1] <= 31)
		return 0;
	if (p[0] == 192 &&
	    (p[1] == 168 || p[1] == 0 || (p[1] == 88 && p[2] == 99)))
		return 0;
	if (p[0] == 198 &&
	    (p[1] == 18 || p[1] == 19 || (p[1] == 51 && p[2] == 100)))
		return 0;
	if (p[0] == 203 && p[1] == 0 && p[2] == 113)
		return 0;
	return 1;
}
#endif
