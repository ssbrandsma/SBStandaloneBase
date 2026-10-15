#include "remote_framebuffer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief Run the program entry point.
 * @return Process exit status.
 */
int main(void)
{
	char e[160];
	RemoteFramebufferLayout l = {
		240, 320, 640, 16, 480, 320, 11, 5, 5, 6, 0, 5, 307200};
	RemoteFramebuffer fb;
	unsigned char a[16] = {0}, b[16] = {0};
	assert(remote_fb_validate_layout(&l, e, sizeof e) == 0);
	l.bpp = 32;
	assert(remote_fb_validate_layout(&l, e, sizeof e) != 0);
	l.bpp = 16;
	l.stride = 100;
	assert(remote_fb_validate_layout(&l, e, sizeof e) != 0);
	l.stride = 480;
	l.yoffset = 640;
	assert(remote_fb_validate_layout(&l, e, sizeof e) != 0);
	l.yoffset = 320;
	l.memory_length = 200000;
	assert(remote_fb_validate_layout(&l, e, sizeof e) != 0);
	remote_fb_init(&fb);
	assert(remote_fb_accept_pixels(&fb, "fb1", a, sizeof a) == 1);
	assert(fb.sequence == 1);
	assert(remote_fb_accept_pixels(&fb, "fb1", a, sizeof a) == 0);
	assert(fb.sequence == 1);
	b[7] = 1;
	assert(remote_fb_accept_pixels(&fb, "fb1", b, sizeof b) == 1);
	assert(fb.sequence == 2);
	assert(remote_fb_accept_pixels(&fb, "fb0", b, sizeof b) == 1);
	assert(fb.sequence == 3);
	remote_fb_free(&fb);
	puts("remote-framebuffer-tests-ok");
	return 0;
}
