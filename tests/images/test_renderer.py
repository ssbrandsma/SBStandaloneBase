#!/usr/bin/env python3
"""Validate real CLI output by decoding PNG pixels without imaging libraries."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib


def decode(png):
    assert png[:8] == b'\x89PNG\r\n\x1a\n'
    pos, compressed = 8, b''
    while pos < len(png):
        size = struct.unpack('!I', png[pos:pos + 4])[0]
        kind = png[pos + 4:pos + 8]
        data = png[pos + 8:pos + 8 + size]
        assert zlib.crc32(kind + data) == struct.unpack('!I', png[pos + 8 + size:pos + 12 + size])[0]
        if kind == b'IHDR':
            width, height, depth, color, *_ = struct.unpack('!IIBBBBB', data)
            assert depth == 8 and color == 6
        elif kind == b'IDAT':
            compressed += data
        pos += size + 12
    raw = zlib.decompress(compressed)
    rows, last = [], bytearray(width * 4)
    stride = width * 4
    for y in range(height):
        filter_type = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a, b, c = (row[x - 4] if x >= 4 else 0), last[x], (last[x - 4] if x >= 4 else 0)
            p = a + b - c
            paeth = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c
            row[x] = (row[x] + [0, a, b, (a + b) // 2, paeth][filter_type]) & 255
        rows.append(row)
        last = row
    return width, height, lambda x, y: tuple(rows[y][x * 4:x * 4 + 4])


def main():
    command = sys.argv[1:]
    with tempfile.TemporaryDirectory() as d:
        root = pathlib.Path(d)
        source, output = root / 'input.svg', root / 'output.png'

        def render(svg, code=0, width=64, height=64):
            source.write_text(svg)
            output.unlink(missing_ok=True)
            result = subprocess.run(command + ['--input', str(source), '--output', str(output),
                                               '--width', str(width), '--height', str(height)], capture_output=True)
            assert result.returncode == code, (result.returncode, result.stderr)
            if code:
                assert not output.exists()
                return
            assert b'peak_rss=' in result.stderr
            return decode(output.read_bytes())

        w, h, pixel = render('<svg viewBox="0 0 100 50"><rect width="100" height="50" fill="red"/></svg>')
        assert (w, h) == (64, 64) and pixel(32, 32) == (255, 0, 0, 255)
        assert pixel(32, 4)[3] == 0 and pixel(32, 60)[3] == 0
        _, _, pixel = render('<svg width="100" height="100"><circle cx="50" cy="50" r="40" fill="#00ff00" opacity="0.5"/></svg>')
        assert pixel(0, 0)[3] == 0 and 125 <= pixel(32, 32)[3] <= 129
        _, _, pixel = render('<svg viewBox="10 20 100 100"><defs><linearGradient id="g"><stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/></linearGradient></defs><path d="M10 20h100v100H10z" fill="url(#g)"/></svg>')
        assert pixel(2, 32)[0] > pixel(60, 32)[0] and pixel(60, 32)[2] > pixel(2, 32)[2]
        render('<svg><rect width="20" height="10" fill="rebeccapurple"/></svg>')
        render('<svg viewBox="0 0 100 100"><g transform="translate(25 25)"><rect width="50" height="50" fill="blue" stroke="red"/></g></svg>')
        for invalid in ['', 'garbage', '<svg>', '<svg><rect></svg>', '<svg/>',
                        '<svg><text>hello</text></svg>', '<svg><style>path{fill:red}</style></svg>',
                        '<svg><image href="https://example.invalid/a"/></svg>',
                        '<!DOCTYPE svg><svg/>', '<svg><path fill="url(https://example.invalid/a)"/></svg>',
                        '<svg><path mask="url(#x)"/></svg>', '<svg><clipPath/></svg>',
                        '<svg>' + ' ' * (1024 * 1024) + '</svg>']:
            render(invalid, 65)
        render('<svg/>', 64, width=513)
        render('<svg viewBox="0 0 1 1">' + '<g>' * 65 + '<rect width="1" height="1"/>' + '</g>' * 65 + '</svg>', 65)
        render('<svg>' + '<rect width="1" height="1"/>' * 4100 + '</svg>', 65)
        render('<svg width="10" height="10"><path d="M0 0 ' + 'L1 1 ' * 12000 + '"/></svg>', 75)
        render('<svg><rect width="1" height="1" style="filter:url(#f)"/></svg>', 65)
        render('<svg viewBox="0 0 100 100"><rect width="100" height="100"/></svg>', width=512, height=512)
        assert not list(root.glob('*.tmp.*'))
    print('svg-renderer-tests-ok')


if __name__ == '__main__':
    main()
