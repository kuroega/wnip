#!/usr/bin/env python3
"""Draw an original Heian-inspired folding-fan icon, using only Python's stdlib.

The hiogi-style fan, vermilion sun, cloud strokes and indigo/gold palette are
original vector-like geometry, not traced or copied from historical artwork.
"""
import math
import os
import struct
import zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 4  # box-filter supersampling

# Coordinates are fractions of the square, shared by every icon size.
FAN_OUTER = [(.50, .86), (.12, .44), (.16, .37), (.24, .30), (.34, .24),
             (.45, .20), (.55, .20), (.66, .24), (.76, .30), (.84, .37), (.88, .44)]
FAN_INNER = [(.50, .79), (.19, .45), (.23, .39), (.30, .34), (.38, .29),
             (.46, .26), (.54, .26), (.62, .29), (.70, .34), (.77, .39), (.81, .45)]
RIB_TIPS = [(.17, .38), (.25, .32), (.35, .26), (.50, .23),
            (.65, .26), (.75, .32), (.83, .38)]

INDIGO = (20, 36, 67)
GOLD = (228, 181, 95)
VERMILION = (181, 62, 48)
PARCHMENT = (249, 227, 177)


def lerp(a, b, t):
    return a + (b - a) * t


def rrect_dist(x, y, radius):
    dx = abs(x - .5) - (.5 - radius)
    dy = abs(y - .5) - (.5 - radius)
    return math.hypot(max(dx, 0), max(dy, 0)) + min(max(dx, dy), 0) - radius


def in_poly(x, y, polygon):
    inside = False
    previous = polygon[-1]
    for current in polygon:
        x0, y0 = previous
        x1, y1 = current
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            inside = not inside
        previous = current
    return inside


def segment_dist2(x, y, start, end):
    x0, y0 = start
    vx, vy = end[0] - x0, end[1] - y0
    t = max(0.0, min(1.0, ((x - x0) * vx + (y - y0) * vy) / (vx * vx + vy * vy)))
    return (x - x0 - t * vx) ** 2 + (y - y0 - t * vy) ** 2


def render(size):
    """Return top-down RGBA rows (0..1 floats), antialiased at every size."""
    n = size * SS
    samples = []
    for sy in range(n):
        y = (sy + .5) / n
        row = []
        for sx in range(n):
            x = (sx + .5) / n
            edge = rrect_dist(x, y, .19)
            if edge > 0:
                row.append((0, 0, 0, 0))
                continue

            # Deep indigo lacquer with a restrained gold keyline.
            shade = .8 + .2 * (1 - y)
            color = tuple(int(v * shade) for v in INDIGO)
            if -.024 < edge < -.011:
                color = GOLD
            elif edge <= -.024 and y < .35:
                # A small gold cloud above the fan, visible in large icons.
                if size >= 32 and .12 < y < .21 and .29 < x < .71:
                    waves = [((.32, .185), (.40, .185)),
                             ((.40, .185), (.45, .145)),
                             ((.45, .145), (.55, .145)),
                             ((.55, .145), (.60, .185)),
                             ((.60, .185), (.68, .185))]
                    if any(segment_dist2(x, y, a, b) < .000035 for a, b in waves):
                        color = (202, 157, 83)

            # The darker vermilion edge frames the cream-coloured fan leaves.
            if in_poly(x, y, FAN_OUTER):
                color = (124, 50, 47)
                if in_poly(x, y, FAN_INNER):
                    t = max(0, min(1, (y - .25) / .55))
                    color = tuple(int(lerp(v, v * .83, t)) for v in PARCHMENT)
                    # Fine gilded slats converge at the fan's pivot.
                    rib_width = .000052 if size >= 32 else .000095
                    if any(segment_dist2(x, y, (.50, .83), tip) < rib_width
                           for tip in RIB_TIPS):
                        color = (178, 130, 71)
                    # Vermilion sun sits on top of the leaf decoration.
                    if (x - .50) ** 2 + (y - .425) ** 2 < .112 ** 2:
                        color = VERMILION

            # One prominent rivet reads cleanly even at 16 x 16.
            d2 = (x - .50) ** 2 + (y - .81) ** 2
            if d2 < .031 ** 2:
                color = GOLD
            if d2 < .012 ** 2:
                color = (104, 55, 48)
            row.append(tuple(v / 255 for v in color) + (1.0,))
        samples.append(row)

    rows = []
    for y in range(size):
        row = []
        for x in range(size):
            channels = [0.0, 0.0, 0.0, 0.0]
            for dy in range(SS):
                for dx in range(SS):
                    pixel = samples[y * SS + dy][x * SS + dx]
                    for ch in range(4):
                        channels[ch] += pixel[ch]
            row.append(tuple(v / (SS * SS) for v in channels))
        rows.append(row)
    return rows


def encode_ico_image(rows, size):
    """BMP/DIB payload for Windows' small icon sizes."""
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = rows[y][x]
            xor += bytes((int(b * 255 + .5), int(g * 255 + .5),
                          int(r * 255 + .5), int(a * 255 + .5)))
    row_bytes = ((size + 31) // 32) * 4
    andmask = bytearray(row_bytes * size)
    for y in range(size):
        for x in range(size):
            if rows[y][x][3] < .5:
                andmask[y * row_bytes + (x >> 3)] |= 0x80 >> (x & 7)
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0,
                         len(xor) + len(andmask), 0, 0, 0, 0)
    return header + xor + andmask


def encode_png(rows, size):
    """PNG payload for large Vista+ icon sizes (also useful for previews)."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for r, g, b, a in row:
            raw += bytes((int(r * 255 + .5), int(g * 255 + .5),
                          int(b * 255 + .5), int(a * 255 + .5)))

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))

    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = os.path.join(root, 'res', 'wnip.ico')
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    payloads = []
    for size in SIZES:
        rows = render(size)
        payloads.append((size, encode_png(rows, size) if size >= 128 else
                         encode_ico_image(rows, size)))
    offset = 6 + 16 * len(payloads)
    entries = bytearray()
    data = bytearray()
    for size, payload in payloads:
        dimension = 0 if size == 256 else size
        entries += struct.pack('<BBBBHHII', dimension, dimension, 0, 0, 1, 32,
                               len(payload), offset)
        offset += len(payload)
        data += payload
    with open(out_path, 'wb') as stream:
        stream.write(struct.pack('<HHH', 0, 1, len(payloads)) + entries + data)
    print('wrote %s (%d sizes, %d bytes)' %
          (out_path, len(payloads), os.path.getsize(out_path)))


if __name__ == '__main__':
    main()
