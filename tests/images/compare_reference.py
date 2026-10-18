#!/usr/bin/env python3
"""Optional host-only comparison against installed librsvg/Cairo via ctypes."""
import ctypes as c
import pathlib
import subprocess
import sys

from test_renderer import decode


class Viewport(c.Structure):
    _fields_ = [('x', c.c_double), ('y', c.c_double), ('width', c.c_double), ('height', c.c_double)]


def main():
    renderer, source = sys.argv[1:3]
    source = pathlib.Path(source)
    nano = source.with_suffix('.nano.png')
    reference = source.with_suffix('.reference.png')
    subprocess.run([renderer, '--input', str(source), '--output', str(nano)], check=True)
    cairo = c.CDLL('libcairo.so.2')
    rsvg = c.CDLL('librsvg-2.so.2')
    objects = c.CDLL('libgobject-2.0.so.0')
    cairo.cairo_image_surface_create.argtypes = [c.c_int, c.c_int, c.c_int]
    cairo.cairo_image_surface_create.restype = c.c_void_p
    cairo.cairo_create.argtypes = [c.c_void_p]
    cairo.cairo_create.restype = c.c_void_p
    cairo.cairo_surface_write_to_png.argtypes = [c.c_void_p, c.c_char_p]
    cairo.cairo_destroy.argtypes = [c.c_void_p]
    cairo.cairo_surface_destroy.argtypes = [c.c_void_p]
    rsvg.rsvg_handle_new_from_file.argtypes = [c.c_char_p, c.c_void_p]
    rsvg.rsvg_handle_new_from_file.restype = c.c_void_p
    rsvg.rsvg_handle_render_document.argtypes = [c.c_void_p, c.c_void_p, c.POINTER(Viewport), c.c_void_p]
    rsvg.rsvg_handle_render_document.restype = c.c_int
    objects.g_object_unref.argtypes = [c.c_void_p]
    surface = cairo.cairo_image_surface_create(0, 320, 320)
    context = cairo.cairo_create(surface)
    handle = rsvg.rsvg_handle_new_from_file(str(source).encode(), None)
    assert handle and rsvg.rsvg_handle_render_document(handle, context, c.byref(Viewport(0, 0, 320, 320)), None)
    assert cairo.cairo_surface_write_to_png(surface, str(reference).encode()) == 0
    objects.g_object_unref(handle)
    cairo.cairo_destroy(context)
    cairo.cairo_surface_destroy(surface)
    _, _, a = decode(nano.read_bytes())
    _, _, b = decode(reference.read_bytes())
    error, different = 0, 0
    for y in range(320):
        for x in range(320):
            p, q = a(x, y), b(x, y)
            # Compare premultiplied channels so transparent RGB is immaterial.
            p = tuple(v * p[3] / 255 for v in p[:3]) + (p[3],)
            q = tuple(v * q[3] / 255 for v in q[:3]) + (q[3],)
            differences = [abs(v - w) for v, w in zip(p, q)]
            error += sum(differences)
            different += max(differences) > 16
    print(source.name, 'mean-channel-error=%.3f/255' % (error / (320 * 320 * 4)),
          'pixels-differing-over-16=%.2f%%' % (100 * different / (320 * 320)))
    print('reference:', reference, 'NanoSVG:', nano)


if __name__ == '__main__':
    main()
