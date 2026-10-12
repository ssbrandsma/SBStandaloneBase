#ifndef SB_REMOTE_FRAMEBUFFER_H
#define SB_REMOTE_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned width, height, stride, page, yoffset;
  unsigned red_offset, red_length, green_offset, green_length;
  unsigned blue_offset, blue_length;
  uint64_t sequence, captured_ms, capture_us;
  const char *device;
  const unsigned char *pixels;
  size_t length;
  int changed, consistent;
} RemoteFrame;

typedef struct {
  unsigned char *current, *previous;
  size_t capacity, previous_length;
  uint64_t sequence;
  uint64_t last_capture_ms;
  RemoteFrame latest;
  char previous_source[24];
  char error[160];
} RemoteFramebuffer;
typedef struct {
  unsigned width,height,virtual_height,bpp,stride,yoffset;
  unsigned red_offset,red_length,green_offset,green_length,blue_offset,blue_length;
  size_t memory_length;
} RemoteFramebufferLayout;

void remote_fb_init(RemoteFramebuffer *fb);
void remote_fb_free(RemoteFramebuffer *fb);
int remote_fb_capture(RemoteFramebuffer *fb, const char *source, RemoteFrame *out);
const char *remote_fb_error(const RemoteFramebuffer *fb);
int remote_fb_validate_layout(const RemoteFramebufferLayout *layout,char *error,size_t error_size);
int remote_fb_accept_pixels(RemoteFramebuffer *fb,const char *source,const unsigned char *pixels,size_t length);

#endif
