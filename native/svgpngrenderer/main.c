/*
 * Local SVG-to-PNG conversion command used by sbwebserver's image module.
 * NanoSVG supplies parsing/rasterization and stb supplies PNG encoding.
 * This short-lived process has no networking, cache or service
 * responsibilities.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "raster.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg/nanosvgrast.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"

#define INPUT_LIMIT (1024u * 1024u)
#define OUTPUT_LIMIT (2u * 1024u * 1024u)

/** Return a documented CLI error without exposing SVG contents. */
static int fail(int code, const char *reason)
{
	fprintf(stderr, "svgpngrenderer: %s\n", reason);
	return code;
}

/** Validate a well-nested, deliberately limited XML/SVG vocabulary. */
static int supported_svg(const char *s)
{
	const char *allowed[] = {"svg",
				 "g",
				 "defs",
				 "path",
				 "rect",
				 "circle",
				 "ellipse",
				 "line",
				 "polyline",
				 "polygon",
				 "linearGradient",
				 "radialGradient",
				 "stop",
				 "title",
				 "desc"};
	char stack[64][32];
	unsigned depth = 0, elements = 0, roots = 0;
	const char *p = s;
	while (*p) {
		char name[32];
		size_t n = 0;
		int closing = 0, self = 0, known = 0;
		if (*p != '<') {
			if (!depth && !isspace((unsigned char)*p))
				return 0;
			p++;
			continue;
		}
		if (!strncmp(p, "<!--", 4)) {
			p = strstr(p + 4, "-->");
			if (!p)
				return 0;
			p += 3;
			continue;
		}
		if (!strncmp(p, "<?xml ", 6) && !roots) {
			p = strstr(p + 6, "?>");
			if (!p)
				return 0;
			p += 2;
			continue;
		}
		p++;
		if (*p == '/') {
			closing = 1;
			p++;
		}
		while (isalnum((unsigned char)*p)) {
			if (n + 1 >= sizeof name)
				return 0;
			name[n++] = *p++;
		}
		name[n] = 0;
		for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
			if (!strcmp(name, allowed[i]))
				known = 1;
		if (!known || ++elements > 4096)
			return 0;
		if (closing) {
			while (isspace((unsigned char)*p))
				p++;
			if (*p++ != '>' || !depth ||
			    strcmp(stack[--depth], name))
				return 0;
			continue;
		}
		if (!depth && (strcmp(name, "svg") || roots++))
			return 0;
		while (*p) {
			char attr[64];
			const char *value;
			size_t len;
			char quote;
			while (isspace((unsigned char)*p))
				p++;
			if (*p == '/' && p[1] == '>') {
				self = 1;
				p += 2;
				break;
			}
			if (*p == '>') {
				p++;
				break;
			}
			n = 0;
			while (isalnum((unsigned char)*p) || *p == ':' ||
			       *p == '-' || *p == '_') {
				if (n + 1 >= sizeof attr)
					return 0;
				attr[n++] = *p++;
			}
			attr[n] = 0;
			while (isspace((unsigned char)*p))
				p++;
			if (!n || *p++ != '=')
				return 0;
			while (isspace((unsigned char)*p))
				p++;
			quote = *p++;
			if (quote != '\'' && quote != '"')
				return 0;
			value = p;
			while (*p && *p != quote) {
				if (*p == '<' || *p == '&')
					return 0;
				p++;
			}
			if (!*p)
				return 0;
			len = (size_t)(p - value);
			p++;
			if (!strcmp(attr, "class") || !strcmp(attr, "filter") ||
			    !strcmp(attr, "mask") ||
			    !strcmp(attr, "clip-path") ||
			    !strncmp(attr, "on", 2))
				return 0;
			if ((!strcmp(attr, "href") ||
			     !strcmp(attr, "xlink:href")) &&
			    (!len || *value != '#'))
				return 0;
			if (!strcmp(attr, "style")) {
				const char *unsupported[] = {"mask",
							     "filter",
							     "clip-path",
							     "font",
							     "mix-blend",
							     "vector-effect"};
				for (size_t i = 0;
				     i <
				     sizeof unsupported / sizeof unsupported[0];
				     i++) {
					const char *found =
						strstr(value, unsupported[i]);
					if (found && found < value + len)
						return 0;
				}
			}
			for (const char *q = value; q + 4 <= value + len; q++) {
				if (!memcmp(q, "url(", 4)) {
					q += 4;
					while (q < value + len &&
					       (isspace((unsigned char)*q) ||
						*q == '\'' || *q == '"'))
						q++;
					if (q >= value + len || *q != '#')
						return 0;
				}
			}
		}
		if (!self) {
			if (depth >= 64)
				return 0;
			strcpy(stack[depth++], name);
		}
	}
	return roots == 1 && depth == 0;
}

/** Parse an exact bounded decimal output dimension. */
static int dimension(const char *s)
{
	char *end;
	long value;
	if (!*s || strspn(s, "0123456789") != strlen(s))
		return 0;
	value = strtol(s, &end, 10);
	return !*end && value >= 16 && value <= 512 ? (int)value : 0;
}

/** Convert one local SVG into an atomic RGBA PNG, with bounded resources. */
int main(int argc, char **argv)
{
	const char *input = NULL, *output = NULL;
	char *svg = NULL, temporary[4096];
	unsigned char *pixels = NULL, *png = NULL;
	NSVGimage *image = NULL;
	NSVGrasterizer *raster = NULL;
	int width = 320, height = 320, fd = -1, png_size = 0, result = 0;
	struct stat st;
	struct rlimit memory = {32u * 1024u * 1024u, 32u * 1024u * 1024u};
	struct rlimit cpu = {5, 10}, core = {0, 0};
	struct timespec begin, end;
	struct rusage usage;
	clock_gettime(CLOCK_MONOTONIC, &begin);
	if (setrlimit(RLIMIT_AS, &memory) || setrlimit(RLIMIT_CPU, &cpu) ||
	    setrlimit(RLIMIT_CORE, &core))
		return fail(75, "cannot enforce resource limits");
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--input") && i + 1 < argc)
			input = argv[++i];
		else if (!strcmp(argv[i], "--output") && i + 1 < argc)
			output = argv[++i];
		else if (!strcmp(argv[i], "--width") && i + 1 < argc)
			width = dimension(argv[++i]);
		else if (!strcmp(argv[i], "--height") && i + 1 < argc)
			height = dimension(argv[++i]);
		else
			return fail(64,
				    "usage: --input FILE --output FILE "
				    "[--width 16..512 --height 16..512]");
	}
	if (!input || !output || !width || !height)
		return fail(64, "invalid arguments or dimensions");
	fd = open(input, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
		if (fd >= 0)
			close(fd);
		return fail(66, "input is not a readable regular file");
	}
	unsigned char signature[8];
	ssize_t signature_size = pread(fd, signature, sizeof signature, 0);
	if (signature_size == 8 &&
	    (!memcmp(signature, "\211PNG\r\n\032\n", 8) ||
	     (signature[0] == 255 && signature[1] == 216))) {
		/* Large scanline-decoded rasters need more CPU on ARM926 than a
		 * small SVG. Keep a hard bound within the service's 20s deadline. */
		cpu.rlim_cur = 10;
		if (setrlimit(RLIMIT_CPU, &cpu)) {
			close(fd);
			return fail(75, "cannot enforce raster CPU limit");
		}
		if (st.st_size > 8u * 1024u * 1024u) {
			close(fd);
			return fail(65, "raster input exceeds 8 MiB");
		}
		return raster_render(fd, output, (unsigned)width, (unsigned)height);
	}
	if (st.st_size <= 0 || st.st_size > INPUT_LIMIT) {
		close(fd);
		return fail(65, "input is empty or exceeds 1 MiB");
	}
	svg = malloc((size_t)st.st_size + 1);
	if (!svg) {
		close(fd);
		return fail(75, "input allocation failed");
	}
	size_t received = 0;
	while (received < (size_t)st.st_size) {
		ssize_t n =
			read(fd, svg + received, (size_t)st.st_size - received);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		received += (size_t)n;
	}
	close(fd);
	svg[received] = 0;
	if (received != (size_t)st.st_size || memchr(svg, 0, received) ||
	    !supported_svg(svg)) {
		result = fail(65, "malformed SVG or unsupported feature");
		goto done;
	}
	image = nsvgParse(svg, "px", 96);
	if (!image || !image->shapes || !isfinite(image->width) ||
	    !isfinite(image->height) || image->width <= 0 ||
	    image->height <= 0) {
		result = fail(65,
			      "SVG has no renderable shapes or valid viewport");
		goto done;
	}
	unsigned points = 0;
	for (NSVGshape *shape = image->shapes; shape; shape = shape->next) {
		for (NSVGpath *path = shape->paths; path; path = path->next) {
			if (path->npts < 0 ||
			    (unsigned)path->npts > 32768u - points) {
				result =
					fail(75, "SVG geometry limit exceeded");
				goto done;
			}
			points += (unsigned)path->npts;
			for (int i = 0; i < path->npts * 2; i++)
				if (!isfinite(path->pts[i]) ||
				    fabsf(path->pts[i]) > 1e7f) {
					result = fail(
						65, "invalid SVG coordinates");
					goto done;
				}
		}
	}
	pixels = calloc((size_t)width * height, 4);
	raster = nsvgCreateRasterizer();
	if (!pixels || !raster) {
		result = fail(75, "raster allocation failed");
		goto done;
	}
	float scale = fminf(width / image->width, height / image->height);
	nsvgRasterize(raster,
		      image,
		      (width - image->width * scale) / 2,
		      (height - image->height * scale) / 2,
		      scale,
		      pixels,
		      width,
		      height,
		      width * 4);
	png = stbi_write_png_to_mem(
		pixels, width * 4, width, height, 4, &png_size);
	if (!png || png_size <= 0 || png_size > (int)OUTPUT_LIMIT) {
		result = fail(75, "PNG encoding or output limit failed");
		goto done;
	}
	if (snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", output) >=
		    (int)sizeof temporary ||
	    (fd = mkstemp(temporary)) < 0) {
		result = fail(73, "cannot create output");
		goto done;
	}
	size_t written = 0;
	while (written < (size_t)png_size) {
		ssize_t n =
			write(fd, png + written, (size_t)png_size - written);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		written += (size_t)n;
	}
	int close_result = close(fd);
	if (written != (size_t)png_size || close_result ||
	    rename(temporary, output)) {
		unlink(temporary);
		result = fail(73, "cannot publish output");
		goto done;
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	getrusage(RUSAGE_SELF, &usage);
	fprintf(stderr,
		"svgpngrenderer: %dx%d %d bytes %.1f ms peak_rss=%ld KiB\n",
		width,
		height,
		png_size,
		(end.tv_sec - begin.tv_sec) * 1000.0 +
			(end.tv_nsec - begin.tv_nsec) / 1e6,
		usage.ru_maxrss);
done:
	free(png);
	free(pixels);
	nsvgDeleteRasterizer(raster);
	nsvgDelete(image);
	free(svg);
	return result;
}
