#!/usr/bin/env python3
"""PPM -> PNG with no third-party dependencies. Optionally tiles inputs into
a contact sheet with captions omitted (labels are added by the caller)."""
import sys, zlib, struct

def read_ppm(path):
    d = open(path, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while i < len(d) and d[i:i+1].isspace(): i += 1
        if d[i:i+1] == b'#':
            while d[i:i+1] not in (b'\n', b''): i += 1
            continue
        j = i
        while j < len(d) and not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i:i + w*h*3]

def write_png(path, w, h, rgb):
    raw = b''.join(b'\x00' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)

def sheet(paths, out, cols, gap=10, bg=(14,16,22)):
    imgs = [read_ppm(p) for p in paths]
    w, h = imgs[0][0], imgs[0][1]
    rows = (len(imgs) + cols - 1) // cols
    W = cols*w + (cols+1)*gap
    H = rows*h + (rows+1)*gap
    canvas = bytearray(bytes(bg) * (W*H))
    for idx, (iw, ih, data) in enumerate(imgs):
        cx, cy = idx % cols, idx // cols
        ox, oy = gap + cx*(w+gap), gap + cy*(h+gap)
        for y in range(ih):
            dst = ((oy+y)*W + ox)*3
            canvas[dst:dst+iw*3] = data[y*iw*3:(y+1)*iw*3]
    write_png(out, W, H, bytes(canvas))

if __name__ == '__main__':
    if sys.argv[1] == '--sheet':
        sheet(sys.argv[3:], sys.argv[2], cols=3)
    else:
        w, h, d = read_ppm(sys.argv[1]); write_png(sys.argv[2], w, h, d)
