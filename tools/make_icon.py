#!/usr/bin/env python3
"""Generate the SysGlance application/tray icon (src/sysglance.ico + icon.png).

Pure Python + zlib, no dependencies. Renders each size independently at
4x supersampling for clean anti-aliased edges, matching the widget's own
palette: dark rounded box, green activity sparkline with a fill under it,
a thinner blue RAM line on larger sizes, and a cyan "live" dot at the
line's end.
"""

import math
import struct
import zlib

SS = 4  # supersample factor

BG     = (20, 26, 34, 255)     # widget background
BORDER = (58, 74, 94, 255)     # subtle frame
GREEN  = (120, 225, 160, 255)  # CPU line
GREENF = (23, 51, 39, 255)     # fill under the line
BLUE   = (110, 175, 250, 255)  # secondary line (RAM)
CYAN   = (95, 225, 245, 255)   # live endpoint dot

# sparkline points, normalized inside the padded box (x,y in 0..1, y down)
LINE = [(0.05, 0.70), (0.19, 0.55), (0.31, 0.63), (0.45, 0.33),
        (0.59, 0.47), (0.71, 0.22), (0.95, 0.30)]
LINE2 = [(0.05, 0.88), (0.25, 0.80), (0.42, 0.90), (0.60, 0.72),
         (0.78, 0.82), (0.95, 0.66)]


def clamp(v, a, b):
    return a if v < a else (b if v > b else v)


def sd_round_rect(px, py, half, r):
    qx = abs(px) - (half - r)
    qy = abs(py) - (half - r)
    ax = max(qx, 0.0)
    ay = max(qy, 0.0)
    return math.hypot(ax, ay) + min(max(qx, qy), 0.0) - r


def seg_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    if dx == 0 and dy == 0:
        return math.hypot(px - x1, py - y1)
    t = clamp(((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy), 0.0, 1.0)
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def y_at_x(pts, x):
    """y on the polyline at x (piecewise linear)."""
    if x <= pts[0][0]:
        return pts[0][1]
    if x >= pts[-1][0]:
        return pts[-1][1]
    for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
        if x1 <= x <= x2:
            if x2 == x1:
                return y1
            return y1 + (y2 - y1) * (x - x1) / (x2 - x1)
    return pts[-1][1]


def render(size):
    """Return RGBA bytes for the icon at `size`."""
    S = size * SS
    out = bytearray(size * size * 4)

    half = S / 2.0
    rad = max(2.0 * SS, S * 0.17)          # corner radius
    bw = max(1.2 * SS, S * 0.014)          # border thickness
    pad = S * 0.17                          # sparkline box padding
    box = half - pad

    lw = max(1.6 * SS, S * 0.055)          # main line half-thickness
    lw2 = lw * 0.55                        # secondary line
    dot_r = max(2.2 * SS, S * 0.075)       # live dot radius
    dot_r += lw * 0.35

    pts = [(pad + x * (S - 2 * pad), pad + y * (S - 2 * pad)) for x, y in LINE]
    pts2 = [(pad + x * (S - 2 * pad), pad + y * (S - 2 * pad)) for x, y in LINE2]
    show2 = size >= 32

    acc = [0.0] * (size * size * 4)

    for yy in range(S):
        py = yy + 0.5 - half
        row_base = (yy // SS) * size
        for xx in range(S):
            px = xx + 0.5 - half
            d = sd_round_rect(px, py, half, rad)
            cov = clamp(0.5 - d, 0.0, 1.0)          # box coverage (AA edge)
            if cov <= 0:
                continue
            # color inside the box: border near the edge, bg deeper in
            edge = clamp(0.5 - (d + bw), 0.0, 1.0)  # 1 at bg, 0 at border
            r = BORDER[0] + (BG[0] - BORDER[0]) * edge
            g = BORDER[1] + (BG[1] - BORDER[1]) * edge
            b = BORDER[2] + (BG[2] - BORDER[2]) * edge

            lx, ly = px + half, py + half            # coords in full canvas
            inside_fill = (d < -pad * 0.4) and (pts[0][0] <= lx <= pts[-1][0])

            # fill under the main line
            if inside_fill:
                yl = y_at_x(pts, lx)
                if ly > yl:
                    r, g, b = GREENF[0], GREENF[1], GREENF[2]

            # secondary (blue) line
            if show2:
                dm = min(seg_dist(lx, ly, *pts2[i], *pts2[i + 1])
                         for i in range(len(pts2) - 1))
                a = clamp((lw2 - dm) + 0.5, 0.0, 1.0)
                if a > 0:
                    r = BLUE[0] * a + r * (1 - a)
                    g = BLUE[1] * a + g * (1 - a)
                    b = BLUE[2] * a + b * (1 - a)

            # main (green) line
            dm = min(seg_dist(lx, ly, *pts[i], *pts[i + 1])
                     for i in range(len(pts) - 1))
            a = clamp((lw - dm) + 0.5, 0.0, 1.0)
            if a > 0:
                r = GREEN[0] * a + r * (1 - a)
                g = GREEN[1] * a + g * (1 - a)
                b = GREEN[2] * a + b * (1 - a)

            # live dot at the line's end
            dd = math.hypot(lx - pts[-1][0], ly - pts[-1][1])
            a = clamp((dot_r - dd) + 0.5, 0.0, 1.0)
            if a > 0:
                r = CYAN[0] * a + r * (1 - a)
                g = CYAN[1] * a + g * (1 - a)
                b = CYAN[2] * a + b * (1 - a)

            # accumulate into the downsample target (SS x SS average)
            o = (row_base + xx // SS) * 4
            acc[o]     += r * cov
            acc[o + 1] += g * cov
            acc[o + 2] += b * cov
            acc[o + 3] += 255 * cov

    # finish averaging
    n = SS * SS
    for i in range(size * size):
        a = acc[i * 4 + 3]
        if a == 0:
            continue
        out[i * 4]     = min(255, int(acc[i * 4] / n + 0.5))
        out[i * 4 + 1] = min(255, int(acc[i * 4 + 1] / n + 0.5))
        out[i * 4 + 2] = min(255, int(acc[i * 4 + 2] / n + 0.5))
        out[i * 4 + 3] = min(255, int(a / n + 0.5))
    # un-premultiply against background where partially transparent
    for i in range(size * size):
        ai = out[i * 4 + 3]
        if 0 < ai < 255:
            for c in range(3):
                v = out[i * 4 + c] * 255 / ai
                out[i * 4 + c] = min(255, int(v + 0.5))
    return bytes(out)


def png_bytes(size, rgba):
    raw = bytearray()
    stride = size * 4
    for y in range(size):
        raw.append(0)                      # filter: none
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack('>I', len(data)) + tag + data
                + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff))

    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', ihdr)
            + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
            + chunk(b'IEND', b''))


def write_ico(path, sizes):
    blobs = [(s, png_bytes(s, render(s))) for s in sizes]
    with open(path, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(blobs)))       # ICONDIR
        off = 6 + 16 * len(blobs)
        for s, blob in blobs:
            w = 0 if s >= 256 else s
            f.write(struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(blob), off))
            off += len(blob)
        for _, blob in blobs:
            f.write(blob)


if __name__ == '__main__':
    import sys
    out_dir = sys.argv[1] if len(sys.argv) > 1 else '.'
    sizes = [16, 24, 32, 48, 64, 128, 256]
    write_ico(out_dir + '/sysglance.ico', sizes)
    with open(out_dir + '/icon.png', 'wb') as f:      # README preview
        f.write(png_bytes(256, render(256)))
    print('wrote sysglance.ico (%s) and icon.png' % ','.join(map(str, sizes)))
