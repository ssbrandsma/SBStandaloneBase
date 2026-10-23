/* Streaming area downsampling. Storage is O(source width + output width),
 * JPEG multi-scan coefficients are separately capped at 4 MiB.
 * Alpha is averaged premultiplied to avoid dark halos.
 * All coverage weights are integer; no floating-point work per pixel on ARMv5.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <png.h>
#include <jpeglib.h>
#include <jerror.h>
#include <syslog.h>
#include "raster.h"

#define JPEG_COEFFICIENT_LIMIT (4u * 1024u * 1024u)
#define JPEG_SCAN_LIMIT 100

#define SIDE_LIMIT 8192u
#define PIXEL_LIMIT (16u * 1024u * 1024u)

typedef struct Raster Raster;
typedef struct { struct jpeg_error_mgr base; Raster *r; } JpegError;
struct Raster {
    jmp_buf jump;
    FILE *input, *output;
    png_structp reader, writer;
    png_infop read_info, write_info;
    struct jpeg_decompress_struct jpeg;
    JpegError jpeg_error;
    int jpeg_created;
    struct jpeg_progress_mgr progress;
    const char *format, *category;
    int failure_code;
    size_t coefficient_bytes;
    unsigned sw, sh, dw, dh, output_y;
    unsigned char *row, *outrow;
    uint32_t *horizontal;
    uint64_t *sum;
    char temporary[4096];
    const char *reason;
    char detail[JMSG_LENGTH_MAX];
    struct timespec begin;
};

static void failure(Raster *r, const char *category, int code, const char *reason) {
    r->category=category; r->failure_code=code;
    snprintf(r->detail,sizeof r->detail,"%s",reason); r->reason=r->detail;
    longjmp(r->jump,1);
}
static void reject(Raster *r, const char *reason) {
    r->reason = reason;
    longjmp(r->jump, 1);
}
static void png_failure(png_structp p, png_const_charp reason) {
    Raster *r = png_get_error_ptr(p);
    /* libpng's reason remains valid only during this callback. */
    fprintf(stderr, "raster: PNG error: %s\n", reason);
    failure(r, p==r->writer ? "encoding_failure" : "decoder_failure", p==r->writer ? 73 : 74, "PNG decoding/encoding failed");
}
static void png_warning_log(png_structp p, png_const_charp reason) {
    fprintf(stderr, "raster: PNG warning: %s\n", reason);
    if (strstr(reason,"user limit")) failure(png_get_error_ptr(p),"resource_limit",75,reason);
}
static void jpeg_failure(j_common_ptr p) {
    JpegError *e = (JpegError *)p->err;
    char message[JMSG_LENGTH_MAX];
    e->base.format_message(p, message);
    fprintf(stderr, "raster: JPEG error: %s\n", message);
    int code=e->base.msg_code;
    if (code==JERR_OUT_OF_MEMORY || code==JERR_NO_BACKING_STORE)
        failure(e->r,"resource_limit",75,message);
    failure(e->r,"decoder_failure",74,message);
}
static void jpeg_message(j_common_ptr p, int level) {
    if (level < 0) jpeg_failure(p); /* Truncation must not create a cache hit. */
}
static void *allocate(Raster *r, size_t bytes) {
    void *p = calloc(1, bytes);
    if (!p) failure(r,"resource_limit",75,"memory limit reached");
    return p;
}
static void setup(Raster *r, unsigned w, unsigned h, unsigned mw, unsigned mh) {
    if (!w || !h || w > SIDE_LIMIT || h > SIDE_LIMIT ||
        (uint64_t)w * h > PIXEL_LIMIT) failure(r,"resource_limit",75,"image dimension/pixel limit exceeded");
    r->sw = w; r->sh = h; r->dw = w; r->dh = h;
    /* Fit, never crop or enlarge. Cross products avoid rounding the wrong axis. */
    if (w > mw || h > mh) {
        if ((uint64_t)w * mh >= (uint64_t)h * mw) {
            r->dw = mw; r->dh = (unsigned)((uint64_t)h * mw / w);
        } else {
            r->dh = mh; r->dw = (unsigned)((uint64_t)w * mh / h);
        }
        if (!r->dw) r->dw = 1;
        if (!r->dh) r->dh = 1;
    }
}
static void start_output(Raster *r) {
    fprintf(stderr, "raster: source=%ux%u target=%ux%u\n", r->sw, r->sh, r->dw, r->dh);
    r->row = allocate(r, (size_t)r->sw * 4);
    r->outrow = allocate(r, (size_t)r->dw * 4);
    r->horizontal = allocate(r, (size_t)r->dw * 4 * sizeof(uint32_t));
    r->sum = allocate(r, (size_t)r->dw * 4 * sizeof(uint64_t));
    r->writer = png_create_write_struct(PNG_LIBPNG_VER_STRING, r, png_failure, png_warning_log);
    if (!r->writer) failure(r,"resource_limit",75,"cannot allocate PNG writer");
    r->write_info = png_create_info_struct(r->writer);
    if (!r->write_info) failure(r,"resource_limit",75,"cannot allocate PNG output info");
    png_init_io(r->writer, r->output);
    png_set_IHDR(r->writer, r->write_info, r->dw, r->dh, 8,
        PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(r->writer, 3);
    png_write_info(r->writer, r->write_info);
}
static void write_row(Raster *r) {
    uint64_t area = (uint64_t)r->sw * r->sh;
    for (unsigned x = 0; x < r->dw; x++) {
        uint64_t *s = r->sum + x * 4;
        for (unsigned c = 0; c < 3; c++)
            r->outrow[x*4+c] = s[3] ? (unsigned char)((s[c]+s[3]/2)/s[3]) : 0;
        r->outrow[x*4+3] = (unsigned char)((s[3]+area/2)/area);
    }
    png_write_row(r->writer, r->outrow);
    memset(r->sum, 0, (size_t)r->dw * 4 * sizeof(uint64_t));
    r->output_y++;
}
static void consume(Raster *r, unsigned y) {
    memset(r->horizontal, 0, (size_t)r->dw * 4 * sizeof(uint32_t));
    unsigned dx=0, boundary=r->sw, left=0, end=0;
    for (unsigned x = 0; x < r->sw; x++) {
        end += r->dw;
        unsigned char *pixel = r->row + x * 4;
        while (left < end) {
            unsigned right = boundary;
            if (right > end) right = end;
            /* At most 8192*255*255 per horizontal color sum: uint32
             * is sufficient. Avoid millions of soft 64-bit multiplies and
             * integer divisions on the Radio's ARM926. */
            uint32_t weight = (right-left) * pixel[3];
            for (unsigned c = 0; c < 3; c++) r->horizontal[dx*4+c] += pixel[c]*weight;
            r->horizontal[dx*4+3] += weight;
            left = right;
            if (left == boundary) { dx++; boundary += r->sw; }
        }
    }
    unsigned top = y * r->dh, yend = top + r->dh;
    while (top < yend) {
        unsigned bottom = (r->output_y+1)*r->sh;
        if (bottom > yend) bottom = yend;
        for (unsigned i = 0; i < r->dw*4; i++) r->sum[i] += (uint64_t)r->horizontal[i]*(bottom-top);
        top = bottom;
        if (top == (r->output_y+1)*r->sh) write_row(r);
    }
}
static void decode_png(Raster *r, unsigned mw, unsigned mh) {
    r->reader = png_create_read_struct(PNG_LIBPNG_VER_STRING, r, png_failure, png_warning_log);
    if (!r->reader) failure(r,"resource_limit",75,"cannot allocate PNG reader");
    r->read_info = png_create_info_struct(r->reader);
    if (!r->read_info) failure(r,"resource_limit",75,"cannot allocate PNG input info");
    png_set_user_limits(r->reader, SIDE_LIMIT, SIDE_LIMIT);
    png_set_chunk_malloc_max(r->reader, 65536);
    png_set_chunk_cache_max(r->reader, 1);
    png_init_io(r->reader, r->input);
    png_read_info(r->reader, r->read_info);
    unsigned w = png_get_image_width(r->reader, r->read_info);
    unsigned h = png_get_image_height(r->reader, r->read_info);
    setup(r, w, h, mw, mh);
    if (png_get_interlace_type(r->reader, r->read_info) != PNG_INTERLACE_NONE)
        failure(r,"unsupported_format",69,"interlaced PNG requires full-frame storage; unsupported on this device");
    int depth = png_get_bit_depth(r->reader, r->read_info);
    int color = png_get_color_type(r->reader, r->read_info);
    if (depth == 16) png_set_strip_16(r->reader);
    if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(r->reader);
    if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(r->reader);
    int transparency = png_get_valid(r->reader, r->read_info, PNG_INFO_tRNS);
    if (transparency) png_set_tRNS_to_alpha(r->reader);
    if (color == PNG_COLOR_TYPE_GRAY || color == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(r->reader);
    if (!(color & PNG_COLOR_MASK_ALPHA) && !transparency) png_set_add_alpha(r->reader, 255, PNG_FILLER_AFTER);
    png_read_update_info(r->reader, r->read_info);
    if (png_get_rowbytes(r->reader, r->read_info) != (size_t)w*4) reject(r, "unexpected PNG row layout");
    start_output(r);
    for (unsigned y = 0; y < h; y++) { png_read_row(r->reader, r->row, NULL); consume(r,y); }
    png_read_end(r->reader, NULL);
}
static void jpeg_progress(j_common_ptr p) {
    Raster *r=((JpegError *)p->err)->r;
    if (r->jpeg.input_scan_number>JPEG_SCAN_LIMIT)
        failure(r,"resource_limit",75,"JPEG scan limit exceeded");
}
static void decode_jpeg(Raster *r, unsigned mw, unsigned mh) {
    r->jpeg.err = jpeg_std_error(&r->jpeg_error.base);
    r->jpeg_error.r = r;
    r->jpeg_error.base.error_exit = jpeg_failure;
    r->jpeg_error.base.emit_message = jpeg_message;
    jpeg_create_decompress(&r->jpeg); r->jpeg_created = 1;
    /* Includes virtual-array overhead; coefficient storage is preflighted below.
     * libjpeg-turbo has no backing-store support: excess fails, never swaps. */
    r->jpeg.mem->max_memory_to_use = JPEG_COEFFICIENT_LIMIT + 1024*1024;
    r->progress.progress_monitor=jpeg_progress;
    r->jpeg.progress=&r->progress;
    jpeg_stdio_src(&r->jpeg, r->input);
    jpeg_read_header(&r->jpeg, TRUE);
    setup(r, r->jpeg.image_width, r->jpeg.image_height, mw, mh);
    fprintf(stderr,"raster: format=jpeg source=%ux%u progressive=%d arithmetic=%d components=%d sampling=",
        r->sw,r->sh,r->jpeg.progressive_mode,r->jpeg.arith_code,r->jpeg.num_components);
    for (int i=0;i<r->jpeg.num_components;i++) {
        jpeg_component_info *c=&r->jpeg.comp_info[i];
        fprintf(stderr,"%s%dx%d",i ? "," : "",c->h_samp_factor,c->v_samp_factor);
        /* Virtual coefficient arrays are rounded to whole MCU dimensions. */
        uint64_t blocks=((c->width_in_blocks+c->h_samp_factor-1)/c->h_samp_factor*c->h_samp_factor);
        blocks*=((c->height_in_blocks+c->v_samp_factor-1)/c->v_samp_factor*c->v_samp_factor);
        if (jpeg_has_multiple_scans(&r->jpeg)) {
            uint64_t bytes=blocks*sizeof(JBLOCK);
            if (bytes>JPEG_COEFFICIENT_LIMIT-r->coefficient_bytes) {
                fprintf(stderr,"\n");
                failure(r,"resource_limit",75,"JPEG coefficient memory limit exceeded");
            }
            r->coefficient_bytes+=(size_t)bytes;
        }
    }
    fprintf(stderr," coefficient_bytes=%zu limit=%u\n",r->coefficient_bytes,JPEG_COEFFICIENT_LIMIT);
    if (r->jpeg.arith_code)
        failure(r,"unsupported_format",69,"arithmetic JPEG is unsupported");
    if (r->jpeg.jpeg_color_space != JCS_RGB && r->jpeg.jpeg_color_space != JCS_YCbCr && r->jpeg.jpeg_color_space != JCS_GRAYSCALE)
        failure(r,"unsupported_format",69,"unsupported JPEG color space");
    /* DCT scaling removes most pixels before they ever enter our row buffers. */
    unsigned dw = r->dw, dh = r->dh;
    r->jpeg.scale_num = 1; r->jpeg.scale_denom = 1;
    for (unsigned d = 2; d <= 8; d *= 2)
        if ((r->jpeg.image_width+d-1)/d >= dw && (r->jpeg.image_height+d-1)/d >= dh) r->jpeg.scale_denom = d;
    r->jpeg.out_color_space = JCS_RGB;
    r->jpeg.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&r->jpeg);
    r->sw = r->jpeg.output_width; r->sh = r->jpeg.output_height;
    start_output(r);
    for (unsigned y = 0; y < r->sh; y++) {
        JSAMPROW row = r->row;
        if (jpeg_read_scanlines(&r->jpeg, &row, 1) != 1) reject(r, "truncated JPEG");
        for (unsigned x = r->sw; x-- > 0;) {
            unsigned char red=r->row[x*3], green=r->row[x*3+1], blue=r->row[x*3+2];
            r->row[x*4]=red; r->row[x*4+1]=green; r->row[x*4+2]=blue; r->row[x*4+3]=255;
        }
        consume(r,y);
    }
    jpeg_finish_decompress(&r->jpeg);
}
int raster_render(int fd, const char *output, unsigned mw, unsigned mh) {
    if (mw > 320) mw = 320;
    if (mh > 240) mh = 240;
    Raster *r = calloc(1, sizeof *r);
    if (!r) { close(fd); return 75; }
    int result = 65;
    r->failure_code=65; r->category="invalid_image"; r->format="unknown";
    clock_gettime(CLOCK_MONOTONIC,&r->begin);
    if (setjmp(r->jump)) goto done;
    r->input = fdopen(fd, "rb");
    if (!r->input) { close(fd); reject(r,"cannot open input stream"); }
    if (snprintf(r->temporary,sizeof r->temporary,"%s.tmp.XXXXXX",output) >= (int)sizeof r->temporary)
        reject(r,"output path too long");
    int outfd = mkstemp(r->temporary);
    if (outfd < 0) failure(r,"encoding_failure",73,"cannot create output");
    r->output = fdopen(outfd,"wb");
    if (!r->output) { close(outfd); failure(r,"encoding_failure",73,"cannot open output stream"); }
    unsigned char signature[8];
    if (fread(signature,1,8,r->input)!=8 || fseek(r->input,0,SEEK_SET)) reject(r,"truncated input");
    if (!png_sig_cmp(signature,0,8)) { r->format="png"; decode_png(r,mw,mh); }
    else if (signature[0]==255 && signature[1]==216) { r->format="jpeg"; decode_jpeg(r,mw,mh); }
    else failure(r,"unsupported_format",69,"unsupported raster signature");
    if (r->output_y != r->dh) reject(r,"incomplete resized image");
    png_write_end(r->writer,NULL);
    int closed = fclose(r->output); r->output=NULL;
    if (closed || rename(r->temporary,output)) failure(r,"encoding_failure",73,"cannot publish output");
    struct rusage usage; getrusage(RUSAGE_SELF,&usage);
    struct timespec end; clock_gettime(CLOCK_MONOTONIC,&end);
    fprintf(stderr,"raster: decoded=%ux%u output=%ux%u row_working_bytes=%zu peak_rss=%ld KiB elapsed_ms=%ld\n",
        r->sw,r->sh,r->dw,r->dh,(size_t)r->sw*4+r->dw*(4+4*4+4*8),usage.ru_maxrss,
        (long)((end.tv_sec-r->begin.tv_sec)*1000+(end.tv_nsec-r->begin.tv_nsec)/1000000));
    result=0;
done:;
    const char *trace_id=getenv("SB_IMAGE_TRACE_ID");
    if (!trace_id) trace_id="-";
    if (r->reason) {
        result=r->failure_code;
        fprintf(stderr,"raster: category=%s format=%s code=%d reason=%s\n",r->category,r->format,result,r->reason);
        syslog(LOG_WARNING,"HTTPTRACE event=image_decode id=%.80s category=%s format=%s code=%d reason=%.160s",trace_id,r->category,r->format,result,r->reason);
    } else {
        syslog(LOG_INFO,"HTTPTRACE event=image_decode id=%.80s category=success format=%s source=%ux%u output=%ux%u coefficient_bytes=%zu",trace_id,r->format,r->sw,r->sh,r->dw,r->dh,r->coefficient_bytes);
    }
    if (r->reader) png_destroy_read_struct(&r->reader,&r->read_info,NULL);
    if (r->writer) png_destroy_write_struct(&r->writer,&r->write_info);
    if (r->jpeg_created) jpeg_destroy_decompress(&r->jpeg);
    if (r->input) fclose(r->input);
    if (r->output) fclose(r->output);
    if (*r->temporary) unlink(r->temporary);
    free(r->row); free(r->outrow); free(r->horizontal); free(r->sum); free(r);
    return result;
}
