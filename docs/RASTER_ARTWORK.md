# Bounded PNG and JPEG artwork

`ImageRender.artworkUrl` now routes remote PNG/JPEG/SVG artwork through
`/api/image/render`. The display adapter explicitly marks extensionless URLs
as `image/*`. Shared transport calls for audio, playlists and JSON/XML remain
unchanged; the Radio's actual stream-routing behavior is regression-tested.
The existing URL validation, public-address
checks, redirect limit, isolated worker, seven-day cache and atomic publication
also apply to raster images. Cache keys include the source URL, requested size
and converter version. Local resources and already-local renderer URLs pass
through unchanged.

PNG/JPEG output fits within 320 by 240 pixels, preserving aspect ratio without
cropping or enlarging smaller images. A square image therefore becomes at most
240 by 240. Requested smaller sizes remain supported. Output is RGBA PNG.

Downloads stream into the private job file, including chunked and close-framed
HTTP responses. They do not accumulate a complete compressed image in Lua or
in Mongoose's receive buffer. The compressed input limit is 8 MiB, download
deadline eight seconds, image dimension limit 8192 per axis and decoded pixel
limit 16 megapixels. The worker has a twenty-second overall deadline. The
converter retains its 32 MiB address-space limit. The CPU limit is ten seconds
for raster input on ARM926 and five seconds for SVG.
Private job files use extended storage, keeping large compressed originals
off the Radio's ramfs. Without extended storage the input limit drops to
512 KiB and private jobs use `/tmp`. Only one conversion job runs at once.
SVG retains its separate 1 MiB parser limit and existing rendering behavior.

The libpng reader normalizes one scanline at a time. An integer area filter
uses two output-width accumulator rows and alpha-premultiplied color sums,
avoiding both a source-sized bitmap and a destination-sized accumulation grid.
Palette, grayscale, transparency and 16-bit PNG input are handled. Baseline
JPEG uses libjpeg-turbo's 1/2, 1/4 or 1/8 DCT scaling when possible, followed
by the same area filter. JPEG coefficient memory is capped at 1 MiB.

Interlaced PNG, progressive/arithmetic JPEG and CMYK JPEG are explicitly
rejected, with a diagnostic, rather than allocating a complete source frame
or coefficient image. Unsupported/corrupt images are never cached. These
formats need a separate bounded implementation before support is enabled.

Build dependencies reproducibly with `scripts/build-image-libs.sh`. It pins
the exact zlib, libpng and libjpeg-turbo source commits and builds static host
and ARM libraries. CMake host builds need their prefix in `CMAKE_PREFIX_PATH`.
`scripts/build-image-test.sh` builds the standalone host test executables;
`scripts/build-image-arm.sh` builds only the two affected ARM executables.
The full `scripts/build-arm.sh` also links these codecs. Dependency notices
are included in the existing packaged `SVG-THIRD-PARTY-NOTICES` file.

Validation commands (from the project root on Linux/WSL):

```sh
python3 tests/images/test_raster.py build-image-host/svgpngrenderer --marcher build-marcher.png
python3 tests/images/test_raster.py build-arm/svgpngrenderer --qemu /usr/bin/qemu-arm --marcher build-marcher.png
python3 tests/images/test_renderer.py build-image-host/svgpngrenderer
python3 tests/images/test_image_http.py build-image-host/sbwebserver-test build-image-host/svgpngrenderer native/sbwebserver/web --test-network
```

The raster tests cover aspect ratios, no enlargement, exact area averaging,
alpha edges, grayscale, palette transparency, 16-bit input, JPEG DCT scaling,
dimension bombs, corrupt/truncated data and explicit format rejections. HTTP
tests cover streaming inputs above 1 MiB, chunked and close-delimited bodies,
truncated responses, existing SSRF checks, cache behavior and worker isolation.

Marcher Vibe's original Squarespace PNG is 1806 by 1806 and 1,670,983 bytes.
The optimized conversion produced 240 by 240 with 19,704 bytes of explicit row
working storage and approximately 3.4 MiB peak host process RSS. On the Radio,
the Marcher PNG became 53,453 bytes; cold conversion took about 7.1 seconds,
and a subsequent cached HTTP request completed in about 0.4 seconds over SSH.
This firmware reports zero for `ru_maxrss`, so that field is unavailable on
the device rather than an actual zero-memory claim. Measurements appear in
`/tmp/standalonebase/sbwebserver.log`. Download sizes, source host, output
dimensions and renderer errors are logged without source URL query strings.
