"""frame.py in.png out.png: 2x nearest-neighbour upscale inside a rounded dark watch bezel.

Needs pypng, which ships with pebble-tool:  ~/.local/share/uv/tools/pebble-tool/bin/python screenshots/frame.py raw.png screenshots/nearby.png"""
import sys, png

SCALE, PAD, RADIUS = 2, 22, 40
BEZEL = (29, 29, 31, 255)

w, h, rows, _ = png.Reader(filename=sys.argv[1]).asRGBA8()
src = [list(r) for r in rows]
W, H = w * SCALE + 2 * PAD, h * SCALE + 2 * PAD


def inside(x, y, r):  # inside the rounded rectangle of size W x H with corner radius r
    cx = min(max(x, r), W - 1 - r)
    cy = min(max(y, r), H - 1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


out = []
for y in range(H):
    line = []
    for x in range(W):
        sx, sy = (x - PAD) // SCALE, (y - PAD) // SCALE
        if 0 <= sx < w and 0 <= sy < h and x >= PAD and y >= PAD:
            line += src[sy][sx * 4:sx * 4 + 3] + [255]
        elif inside(x, y, RADIUS):
            line += BEZEL
        else:
            line += (0, 0, 0, 0)
    out.append(line)
png.Writer(W, H, alpha=True, greyscale=False).write(open(sys.argv[2], 'wb'), out)
