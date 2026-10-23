#!/usr/bin/env python3
"""Real codec/area-filter tests; fixtures are generated without image libraries."""
import argparse
import pathlib
import struct
import subprocess
import tempfile
import zlib
from test_renderer import decode


def chunk(kind, data):
    return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))


def png(w, h, row, color=6, depth=8, interlace=0, extra=b''):
    raw = b''.join(b'\0' + row(y) for y in range(h))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', w, h, depth, color, 0, 0, interlace))
            + extra + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('renderer')
    parser.add_argument('--marcher')
    parser.add_argument('--lastfm', help='Optional exact Last.fm reproduction JPEG')
    parser.add_argument('--qemu')
    args = parser.parse_args()
    command = ([args.qemu] if args.qemu else []) + [str(pathlib.Path(args.renderer).resolve())]
    fixtures = pathlib.Path(__file__).parent / 'fixtures'
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)

        def render(data, width=320, height=240, good=True):
            source, target = root / 'source', root / 'result.png'
            source.write_bytes(data)
            target.unlink(missing_ok=True)
            result = subprocess.run(command + ['--input', str(source), '--output', str(target),
                                    '--width', str(width), '--height', str(height)], capture_output=True, timeout=20)
            if not good:
                assert result.returncode != 0 and not target.exists(), result.stderr
                assert not list(root.glob('*.tmp.*'))
                return result.stderr
            assert result.returncode == 0, result.stderr
            image = decode(target.read_bytes())
            assert image[0] <= 320 and image[1] <= 240
            assert target.stat().st_size < 512 * 1024
            return image

        red = bytes([240, 20, 30, 255])
        result = render(png(640, 480, lambda y: red * 640))
        assert result[:2] == (320, 240) and bytes(result[2](0, 0)) == red
        assert render(png(480, 640, lambda y: red * 480))[:2] == (180, 240)
        assert render(png(641, 479, lambda y: red * 641))[:2] == (320, 239)
        assert render(png(13, 7, lambda y: red * 13))[:2] == (13, 7)  # Never enlarge.
        assert render(png(8192, 1, lambda y: red * 8192))[:2] == (320, 1)
        assert render(png(640, 480, lambda y: red * 640), 512, 512)[:2] == (320, 240)
        # True area coverage, including transparent pixels; no black/red fringe.
        row = bytes([255, 0, 0, 0, 0, 0, 255, 255]) * 320
        result = render(png(640, 480, lambda y: row))
        assert bytes(result[2](0, 0)) == bytes([0, 0, 255, 128]), bytes(result[2](0, 0))
        row = bytes([0, 0, 0, 255, 255, 255, 255, 255]) * 320
        result = render(png(640, 480, lambda y: row))
        assert bytes(result[2](0, 0)) == bytes([128, 128, 128, 255])
        result = render(png(20, 20, lambda y: bytes([127]) * 20, color=0))
        assert bytes(result[2](0, 0)) == bytes([127, 127, 127, 255])
        palette = chunk(b'PLTE', bytes([0, 255, 0, 0, 0, 255])) + chunk(b'tRNS', bytes([255, 128]))
        result = render(png(20, 20, lambda y: bytes([1]) * 20, color=3, extra=palette))
        assert bytes(result[2](0, 0)) == bytes([0, 0, 255, 128])
        result = render(png(20, 20, lambda y: b'\xff\xff' * 20, color=0, depth=16))
        assert bytes(result[2](0, 0)) == bytes([255, 255, 255, 255])
        result = render((fixtures / 'baseline.jpg').read_bytes())
        assert result[:2] == (320, 240)
        assert all(abs(a-b) < 4 for a, b in zip(result[2](0, 0)[:3], (230, 40, 80)))
        for name in ['progressive.jpg', 'jpeg-444.jpg', 'jpeg-422.jpg', 'jpeg-420-progressive.jpg', 'jpeg-gray.jpg', 'jpeg-exif.jpg']:
            data=(fixtures / name).read_bytes()
            assert data[:2]==b'\xff\xd8'
            assert render(data)[:2] == (320, 240), name
        assert b'category=unsupported_format' in render((fixtures/'jpeg-cmyk.jpg').read_bytes(),good=False)
        failure=render((fixtures/'jpeg-coefficient-limit.jpg').read_bytes(),good=False)
        assert b'category=resource_limit' in failure and b'coefficient memory limit' in failure
        # SOF0/SOF2 marker checks on actual encoded fixture bytes.
        assert b'\xff\xc0' in (fixtures/'baseline.jpg').read_bytes()
        assert b'\xff\xc2' in (fixtures/'progressive.jpg').read_bytes()
        if args.lastfm:
            data=pathlib.Path(args.lastfm).read_bytes()
            assert len(data)==70094 and data[:2]==b'\xff\xd8' and b'\xff\xc2' in data
            assert render(data)[:2]==(242,240)
            assert b'category=decoder_failure' in render(data[:-200],good=False)
        assert b'interlaced' in render(png(20, 20, lambda y: red * 20, interlace=1), good=False)
        render(png(20, 20, lambda y: red * 20)[:-20], good=False)
        render((fixtures / 'baseline.jpg').read_bytes()[:-200], good=False)
        render(b'not an image', good=False)
        assert b'category=resource_limit' in render(png(8193, 1, lambda y: red * 8193), good=False)
        # Dimension bombs are rejected from IHDR, before a pixel buffer exists.
        bomb = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', 8192, 8192, 8, 6, 0, 0, 0)) + chunk(b'IDAT', b'')
        render(bomb, good=False)
        # Output creation failure has its own category, with no published file.
        source=root/'source';source.write_bytes((fixtures/'baseline.jpg').read_bytes())
        failed=subprocess.run(command+['--input',str(source),'--output',str(root/'missing'/'out.png'),
                              '--width','320','--height','240'],capture_output=True,timeout=20)
        assert failed.returncode==73 and b'category=encoding_failure' in failed.stderr
        if args.marcher:
            result = render(pathlib.Path(args.marcher).read_bytes())
            assert result[:2] == (240, 240)
    print('raster resize, area filtering, alpha, bounds, no-upscale and malformed-input tests passed')


if __name__ == '__main__':
    main()
