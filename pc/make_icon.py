"""Draws the SoundBridge icon (PS-blue rounded square, white sound waves)
and writes soundbridge.ico with 256/48/32/24/16 px PNG images.
Standard library only: python make_icon.py OUT.ico"""
import math, struct, sys, zlib

N = 256
SS = 3  # supersampling per axis


def coverage(x, y):
    """RGBA (0..1) at a point of the 256x256 icon."""
    # rounded square
    r = 58.0
    m = 8.0
    qx = max(abs(x - 128) - (128 - m - r), 0.0)
    qy = max(abs(y - 128) - (128 - m - r), 0.0)
    if math.hypot(qx, qy) > r:
        return None
    t = (y - m) / (N - 2 * m)
    top, bot = (0x2A, 0x8C, 0xFF), (0x0A, 0x3F, 0x9E)
    col = [top[i] + (bot[i] - top[i]) * t for i in range(3)]
    # soft highlight at the top left
    hl = max(0.0, 1.0 - math.hypot(x - 70, y - 50) / 170.0) * 0.18
    col = [c + (255 - c) * hl for c in col]
    # sound source dot and three arcs, centred left of the middle
    cx, cy = 86.0, 128.0
    d = math.hypot(x - cx, y - cy)
    ang = math.degrees(math.atan2(y - cy, x - cx))
    white = 0.0
    if d <= 20:
        white = 1.0
    for rad, a in ((52, 1.0), (86, 0.88), (120, 0.72)):
        if abs(d - rad) <= 9 and abs(ang) <= 46:
            white = max(white, a)
        # round caps
        for s in (-46, 46):
            ex = cx + rad * math.cos(math.radians(s))
            ey = cy + rad * math.sin(math.radians(s))
            if math.hypot(x - ex, y - ey) <= 9:
                white = max(white, a)
    col = [c + (255 - c) * white for c in col]
    return col


def render():
    img = [[(0, 0, 0, 0)] * N for _ in range(N)]
    for py in range(N):
        row = img[py]
        for px in range(N):
            acc = [0.0, 0.0, 0.0]
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    c = coverage(px + (sx + 0.5) / SS, py + (sy + 0.5) / SS)
                    if c:
                        hits += 1
                        for i in range(3):
                            acc[i] += c[i]
            if hits:
                row[px] = (acc[0] / hits, acc[1] / hits, acc[2] / hits, 255.0 * hits / (SS * SS))
    return img


def downscale(img, n):
    """Area average of the 256 image (premultiplied)."""
    f = N / n
    out = []
    for y in range(n):
        row = []
        for x in range(n):
            r = g = b = a = 0.0
            cnt = 0
            for yy in range(int(y * f), int((y + 1) * f)):
                for xx in range(int(x * f), int((x + 1) * f)):
                    pr, pg, pb, pa = img[yy][xx]
                    r += pr * pa; g += pg * pa; b += pb * pa; a += pa
                    cnt += 1
            if a > 0:
                row.append((r / a, g / a, b / a, a / cnt))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


def png(img):
    n = len(img)
    raw = b"".join(b"\x00" + bytes(
        v for px in row for v in (int(px[0] + .5), int(px[1] + .5), int(px[2] + .5), int(px[3] + .5)))
        for row in img)
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", n, n, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(out):
    big = render()
    images = [png(big)] + [png(downscale(big, s)) for s in (48, 32, 24, 16)]
    sizes = [256, 48, 32, 24, 16]
    hdr = struct.pack("<HHH", 0, 1, len(images))
    off = 6 + 16 * len(images)
    ents = b""
    for s, data in zip(sizes, images):
        ents += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), off)
        off += len(data)
    with open(out, "wb") as f:
        f.write(hdr + ents + b"".join(images))
    # a preview PNG next to it
    with open(out[:-4] + "_preview.png", "wb") as f:
        f.write(images[0])


if __name__ == "__main__":
    main(sys.argv[1])
