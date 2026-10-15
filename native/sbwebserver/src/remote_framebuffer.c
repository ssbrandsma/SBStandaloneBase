/*
 * Linux framebuffer capture, pixel conversion and image generation.
 *
 * Provides layout validation and LCD frame handling for remote.c's Virtual
 * Radio API. Encapsulates framebuffer-device details beneath the HTTP and
 * remote-session layers; the host tests exercise its layout and pixel logic.
 */

#define _POSIX_C_SOURCE 200809L
#include "remote_framebuffer.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

/**
 * @brief Handle milliseconds.
 * @return Value produced by the operation.
 */
static uint64_t milliseconds(void)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return (uint64_t)t.tv_sec * 1000ULL + (uint64_t)t.tv_usec / 1000ULL;
}

/**
 * @brief Handle microseconds.
 * @return Value produced by the operation.
 */
static uint64_t microseconds(void)
{
	struct timeval t;
	gettimeofday(&t, NULL);
	return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_usec;
}

/**
 * @brief Handle reserve.
 * @param fb Object referenced by fb.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int reserve(RemoteFramebuffer *fb, size_t n)
{
	unsigned char *a, *b;
	if (fb->capacity >= n)
		return 0;
	a = (unsigned char *)realloc(fb->current, n);
	if (!a)
		return -1;
	fb->current = a;
	b = (unsigned char *)realloc(fb->previous, n);
	if (!b)
		return -1;
	fb->previous = b;
	fb->capacity = n;
	return 0;
}

/**
 * @brief Initialize remote fb.
 * @param fb Object referenced by fb.
 */
void remote_fb_init(RemoteFramebuffer *fb)
{
	memset(fb, 0, sizeof *fb);
}

/**
 * @brief Release resources owned by remote fb.
 * @param fb Object referenced by fb.
 */
void remote_fb_free(RemoteFramebuffer *fb)
{
	free(fb->current);
	free(fb->previous);
	memset(fb, 0, sizeof *fb);
}

/**
 * @brief Handle remote fb error.
 * @param fb Object referenced by fb.
 * @return Pointer to the requested object, or `NULL` when unavailable.
 */
const char *remote_fb_error(const RemoteFramebuffer *fb)
{
	return fb->error;
}

/**
 * @brief Handle remote fb validate layout.
 * @param l Object referenced by l.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int remote_fb_validate_layout(const RemoteFramebufferLayout *l,
			      char *e,
			      size_t n)
{
	size_t visible, offset;
	if (l->bpp != 16) {
		snprintf(e, n, "unsupported %u-bit framebuffer", l->bpp);
		return -1;
	}
	if (l->red_offset != 11 || l->red_length != 5 || l->green_offset != 5 ||
	    l->green_length != 6 || l->blue_offset != 0 ||
	    l->blue_length != 5) {
		snprintf(e, n, "unsupported RGB bitfields");
		return -1;
	}
	if (!l->width || !l->height || l->width > 2048 || l->height > 2048 ||
	    l->stride < l->width * 2U) {
		snprintf(e, n, "invalid framebuffer dimensions or stride");
		return -1;
	}
	if (l->yoffset > l->virtual_height ||
	    l->height > l->virtual_height - l->yoffset) {
		snprintf(e, n, "selected page is outside virtual framebuffer");
		return -1;
	}
	visible = (size_t)l->height * l->stride;
	offset = (size_t)l->yoffset * l->stride;
	if (offset > l->memory_length || visible > l->memory_length - offset) {
		snprintf(e, n, "selected page exceeds framebuffer memory");
		return -1;
	}
	return 0;
}

/**
 * @brief Handle remote fb accept pixels.
 * @param fb Object referenced by fb.
 * @param source Input text for source.
 * @param p Input text for p.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int remote_fb_accept_pixels(RemoteFramebuffer *fb,
			    const char *source,
			    const unsigned char *p,
			    size_t n)
{
	int changed;
	if (reserve(fb, n))
		return -1;
	memcpy(fb->current, p, n);
	changed = fb->previous_length != n ||
		  strcmp(fb->previous_source, source) ||
		  memcmp(fb->current, fb->previous, n);
	if (changed) {
		unsigned char *t = fb->previous;
		fb->previous = fb->current;
		fb->current = t;
		fb->previous_length = n;
		snprintf(fb->previous_source,
			 sizeof fb->previous_source,
			 "%s",
			 source);
		fb->sequence++;
	}
	return changed;
}

#ifdef __linux__
/**
 * @brief Handle layout ok.
 * @param f Open stream to process.
 * @param v Object referenced by v.
 * @param e Mutable text buffer for e.
 * @param n Capacity or item count associated with the operation.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int layout_ok(const struct fb_fix_screeninfo *f,
		     const struct fb_var_screeninfo *v,
		     char *e,
		     size_t n)
{
	RemoteFramebufferLayout l = {v->xres,
				     v->yres,
				     v->yres_virtual,
				     v->bits_per_pixel,
				     f->line_length,
				     v->yoffset,
				     v->red.offset,
				     v->red.length,
				     v->green.offset,
				     v->green.length,
				     v->blue.offset,
				     v->blue.length,
				     f->smem_len};
	return remote_fb_validate_layout(&l, e, n) == 0;
}

/**
 * @brief Parse source.
 * @param s Input text for s.
 * @param path Filesystem path to process.
 * @param forced Object referenced by forced.
 * @param page Object referenced by page.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
static int
parse_source(const char *s, const char **path, int *forced, unsigned *page)
{
	*forced = 0;
	*page = 0;
	if (!strcmp(s, "fb0")) {
		*path = "/dev/fb0";
		return 1;
	}
	if (!strcmp(s, "fb1") || !strcmp(s, "auto")) {
		*path = "/dev/fb1";
		return 1;
	}
	if (!strncmp(s, "fb0p", 4) && s[4] >= '0' && s[4] <= '1' && !s[5]) {
		*path = "/dev/fb0";
		*forced = 1;
		*page = (unsigned)(s[4] - '0');
		return 1;
	}
	if (!strncmp(s, "fb1p", 4) && s[4] >= '0' && s[4] <= '1' && !s[5]) {
		*path = "/dev/fb1";
		*forced = 1;
		*page = (unsigned)(s[4] - '0');
		return 1;
	}
	return 0;
}
#endif

/**
 * @brief Handle remote fb capture.
 * @param fb Object referenced by fb.
 * @param source Input text for source.
 * @param out Destination buffer for the result.
 * @return Zero on success; otherwise a nonzero status or error code.
 */
int remote_fb_capture(RemoteFramebuffer *fb,
		      const char *source,
		      RemoteFrame *out)
{
	uint64_t now = milliseconds();
	memset(out, 0, sizeof *out);
	fb->error[0] = 0;
	if (fb->previous_length && fb->last_capture_ms &&
	    now - fb->last_capture_ms < 100 &&
	    !strcmp(fb->previous_source, source)) {
		*out = fb->latest;
		out->changed = 0;
		return 0;
	}
#ifdef __linux__
	const char *path;
	int forced, fd = -1, attempt;
	unsigned forced_page;
	struct fb_fix_screeninfo fix;
	struct fb_var_screeninfo before, after;
	unsigned char *map = MAP_FAILED;
	size_t offset, length;
	uint64_t started = microseconds();
	if (!parse_source(source, &path, &forced, &forced_page)) {
		snprintf(fb->error,
			 sizeof fb->error,
			 "invalid framebuffer source");
		return -1;
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(fb->error,
			 sizeof fb->error,
			 "open %s: %s",
			 path,
			 strerror(errno));
		return -1;
	}
	if (ioctl(fd, FBIOGET_FSCREENINFO, &fix) ||
	    ioctl(fd, FBIOGET_VSCREENINFO, &before)) {
		snprintf(fb->error,
			 sizeof fb->error,
			 "query %s: %s",
			 path,
			 strerror(errno));
		close(fd);
		return -1;
	}
	if (!layout_ok(&fix, &before, fb->error, sizeof fb->error)) {
		close(fd);
		return -1;
	}
	length = (size_t)before.yres * fix.line_length;
	if (reserve(fb, length)) {
		snprintf(fb->error,
			 sizeof fb->error,
			 "capture buffer allocation failed");
		close(fd);
		return -1;
	}
	map = (unsigned char *)mmap(
		NULL, fix.smem_len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		snprintf(fb->error,
			 sizeof fb->error,
			 "map %s: %s",
			 path,
			 strerror(errno));
		close(fd);
		return -1;
	}
	for (attempt = 0; attempt < 2; attempt++) {
		if (ioctl(fd, FBIOGET_VSCREENINFO, &before))
			break;
		if (forced)
			before.yoffset = forced_page * before.yres;
		if (before.yoffset > before.yres_virtual ||
		    before.yres > before.yres_virtual - before.yoffset) {
			snprintf(
				fb->error,
				sizeof fb->error,
				"selected page is outside virtual framebuffer");
			break;
		}
		offset = (size_t)before.yoffset * fix.line_length;
		if (offset > fix.smem_len || length > fix.smem_len - offset) {
			snprintf(fb->error,
				 sizeof fb->error,
				 "selected page exceeds framebuffer memory");
			break;
		}
		memcpy(fb->current, map + offset, length);
		if (ioctl(fd, FBIOGET_VSCREENINFO, &after))
			break;
		if (forced || before.yoffset == after.yoffset) {
			out->consistent = 1;
			break;
		}
	}
	munmap(map, fix.smem_len);
	close(fd);
	if (!out->consistent) {
		if (!fb->error[0])
			snprintf(fb->error,
				 sizeof fb->error,
				 "active framebuffer page changed during "
				 "capture");
		return -1;
	}
	out->changed = remote_fb_accept_pixels(fb, source, fb->current, length);
	out->pixels = fb->previous;
	out->length = length;
	out->width = before.xres;
	out->height = before.yres;
	out->stride = fix.line_length;
	out->yoffset = before.yoffset;
	out->page = before.yres ? before.yoffset / before.yres : 0;
	out->red_offset = before.red.offset;
	out->red_length = before.red.length;
	out->green_offset = before.green.offset;
	out->green_length = before.green.length;
	out->blue_offset = before.blue.offset;
	out->blue_length = before.blue.length;
	out->sequence = fb->sequence;
	out->captured_ms = milliseconds();
	out->capture_us = microseconds() - started;
	out->device = path;
	fb->last_capture_ms = out->captured_ms;
	fb->latest = *out;
	return 0;
#else
	(void)source;
	snprintf(fb->error,
		 sizeof fb->error,
		 "framebuffer capture is unavailable on this host");
	return -1;
#endif
}
