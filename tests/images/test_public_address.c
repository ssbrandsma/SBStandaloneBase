/* Validate the common destination policy used by Mongoose image fetches and
 * sbproxy. */
#include "../../native/common/public_address.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
	const unsigned char unsafe[][4] = {{0, 0, 0, 0},
					   {10, 1, 2, 3},
					   {127, 0, 0, 1},
					   {100, 64, 0, 1},
					   {100, 127, 255, 255},
					   {169, 254, 169, 254},
					   {172, 16, 0, 1},
					   {172, 31, 255, 255},
					   {192, 168, 1, 1},
					   {192, 0, 2, 1},
					   {192, 88, 99, 1},
					   {198, 18, 0, 1},
					   {198, 19, 0, 1},
					   {198, 51, 100, 1},
					   {203, 0, 113, 1},
					   {224, 0, 0, 1},
					   {255, 255, 255, 255}};
	for (unsigned i = 0; i < sizeof unsafe / sizeof unsafe[0]; i++)
		assert(!sb_public_ipv4(unsafe[i]));
	const unsigned char safe[][4] = {{8, 8, 8, 8},
					 {1, 1, 1, 1},
					 {93, 184, 216, 34},
					 {172, 32, 1, 1}};
	for (unsigned i = 0; i < sizeof safe / sizeof safe[0]; i++)
		assert(sb_public_ipv4(safe[i]));
	puts("image-public-address-tests-ok");
	return 0;
}
