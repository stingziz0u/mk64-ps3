#!/usr/bin/env python3
"""ppm2png.py a.ppm [b.ppm ...] -> a.png ... scaled 2x (no dependencies)."""
import sys, zlib, struct
for path in sys.argv[1:]:
    d = open(path, 'rb').read().split(b'\n', 3)
    w, h = map(int, d[1].split()); px = d[3]
    rows = []
    for y in range(h):
        r = px[y * w * 3:(y + 1) * w * 3]
        r2 = b''.join(r[i:i + 3] * 2 for i in range(0, len(r), 3))
        rows += [b'\x00' + r2] * 2
    c = lambda t, x: struct.pack('>I', len(x)) + t + x + struct.pack('>I', zlib.crc32(t + x))
    open(path[:-4] + '.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', w * 2, h * 2, 8, 2, 0, 0, 0))
                                         + c(b'IDAT', zlib.compress(b''.join(rows))) + c(b'IEND', b''))
