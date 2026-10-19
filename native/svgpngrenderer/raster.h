#ifndef SB_RASTER_H
#define SB_RASTER_H
/* Consumes fd. The caller has already checked its type and compressed size. */
int raster_render(int fd, const char *output, unsigned width, unsigned height);
#endif
