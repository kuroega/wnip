#!/usr/bin/env python3
"""Generate res/wnip.ico - a multi-size camera icon, pure stdlib, deterministic."""
import math, os, struct, sys, zlib

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SS = 4  # supersample factor


def lerp(a, b, t):
    return a + (b - a) * t


def rrect_dist(px, py, cx, cy, hx, hy, r):
    dx = abs(px - cx) - (hx - r)
    dy = abs(py - cy) - (hy - r)
    ax, ay = max(dx, 0.0), max(dy, 0.0)
    return math.hypot(ax, ay) + min(max(dx, dy), 0.0) - r


def render(size):
    """Return list of rows of (r,g,b,a) floats 0..1, top-down."""
    n = size * SS
    # palette
    top = (0x3B / 255.0, 0x8C / 255.0, 0xFF / 255.0)
    bot = (0x14 / 255.0, 0x53 / 255.0, 0xB8 / 255.0)
    white = (1.0, 1.0, 1.0)
    lens_in = (0x0E / 255.0, 0x3B / 255.0, 0x86 / 255.0)
    c = n / 2.0
    # geometry (fractions of n)
    bg_r = 0.235 * n
    body_hx, body_hy = 0.325 * n, 0.225 * n
    body_r = 0.085 * n
    body_cy = c + 0.035 * n
    bump_hx, bump_hy = 0.115 * n, 0.055 * n
    bump_cy = body_cy - body_hy + 0.005 * n
    bump_r = 0.035 * n
    lens_r = 0.155 * n
    lens_cy = body_cy
    ring_r = lens_r + 0.030 * n
    dot_r = 0.032 * n
    dot_cx, dot_cy = c + 0.235 * n, body_cy - body_hy + 0.055 * n

    out = []
    for y in range(n):
        row = []
        for x in range(n):
            fx, fy = x + 0.5, y + 0.5
            # background rounded square with vertical gradient
            if rrect_dist(fx, fy, c, c, c - 0.5, c - 0.5, bg_r) <= 0.0:
                t = fy / n
                col = (lerp(top[0], bot[0], t), lerp(top[1], bot[1], t), lerp(top[2], bot[2], t), 1.0)
            else:
                col = (0.0, 0.0, 0.0, 0.0)
            if col[3] > 0.0:
                # camera bump (behind body)
                if rrect_dist(fx, fy, c, bump_cy, bump_hx, bump_hy, bump_r) <= 0.0:
                    col = (white[0], white[1], white[2], 1.0)
                # camera body
                if rrect_dist(fx, fy, c, body_cy, body_hx, body_hy, body_r) <= 0.0:
                    col = (white[0], white[1], white[2], 1.0)
                # lens ring (blue)
                if math.hypot(fx - c, fy - lens_cy) <= ring_r:
                    col = (lerp(top[0], bot[0], 0.25), lerp(top[1], bot[1], 0.25),
                           lerp(top[2], bot[2], 0.25), 1.0)
                # lens glass
                if math.hypot(fx - c, fy - lens_cy) <= lens_r:
                    col = (lens_in[0], lens_in[1], lens_in[2], 1.0)
                # flash dot
                if math.hypot(fx - dot_cx, fy - dot_cy) <= dot_r:
                    col = (lerp(top[0], bot[0], 0.1), lerp(top[1], bot[1], 0.1),
                           lerp(top[2], bot[2], 0.1), 1.0)
            row.append(col)
        out.append(row)

    # downsample SSxSS box filter
    res = []
    for y in range(size):
        row = []
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(SS):
                for sx in range(SS):
                    p = out[y * SS + sy][x * SS + sx]
                    r += p[0]; g += p[1]; b += p[2]; a += p[3]
            k = 1.0 / (SS * SS)
            row.append((r * k, g * k, b * k, a * k))
        res.append(row)
    return res


def encode_ico_image(rows, size):
    """Build BMP (DIB) payload for one image: header + XOR BGRA bottom-up + AND mask."""
    # XOR bitmap: BGRA, bottom-up
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = rows[y][x]
            # premultiply-free straight alpha
            xor += bytes((int(b * 255 + 0.5) & 0xFF,
                          int(g * 255 + 0.5) & 0xFF,
                          int(r * 255 + 0.5) & 0xFF,
                          int(a * 255 + 0.5) & 0xFF))
    # AND mask: 1 bit per pixel, rows padded to 4 bytes, 0 = opaque
    row_bytes = ((size + 31) // 32) * 4
    andmask = bytearray(row_bytes * size)
    for y in range(size):
        for x in range(size):
            if rows[y][x][3] < 0.5:
                andmask[y * row_bytes + (x >> 3)] |= (0x80 >> (x & 7))
    bih = struct.pack('<IiiHHIIiiII',
                      40,          # biSize
                      size,        # biWidth
                      size * 2,    # biHeight (XOR + AND)
                      1,           # biPlanes
                      32,          # biBitCount
                      0,           # biCompression BI_RGB
                      len(xor) + len(andmask),  # biSizeImage
                      0, 0, 0, 0)
    return bytes(bih) + bytes(xor) + bytes(andmask)


def encode_png(rows, size):
    raw = bytearray()
    for y in range(size):
        raw.append(0)
        for x in range(size):
            r, g, b, a = rows[y][x]
            raw += bytes((min(255, int(r * 255 + 0.5)), min(255, int(g * 255 + 0.5)),
                          min(255, int(b * 255 + 0.5)), min(255, int(a * 255 + 0.5))))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)

    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
            + chunk(b'IEND', b''))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out_path = os.path.join(root, 'res', 'wnip.ico')
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    payloads = []
    for s in SIZES:
        rows = render(s)
        # PNG payload for large sizes keeps the file small (Vista+ supports it)
        payloads.append((s, encode_png(rows, s) if s >= 128 else encode_ico_image(rows, s)))
    header = struct.pack('<HHH', 0, 1, len(payloads))
    offset = 6 + 16 * len(payloads)
    entries = b''
    data = b''
    for s, p in payloads:
        w = 0 if s >= 256 else s
        h = 0 if s >= 256 else s
        entries += struct.pack('<BBBBHHII', w, h, 0, 0, 1, 32, len(p), offset)
        offset += len(p)
        data += p
    with open(out_path, 'wb') as f:
        f.write(header + entries + data)
    print('wrote %s (%d sizes, %d bytes)' % (out_path, len(payloads), os.path.getsize(out_path)))


if __name__ == '__main__':
    main()
