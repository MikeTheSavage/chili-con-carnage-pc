#!/usr/bin/env python3
"""Builds a small texture mod from extracted textures, to try run-time replacement (mods/TEXTURES.md).

    python tools\\sample_texture_mod.py <extracted folder> <mods folder>

<extracted folder> is the output of extract_textures.py (it holds index.csv). The mod is written to
<mods folder>\\sample_textures\\textures\\<key>.png: each picture four times the size of the game's,
smoothly enlarged and recoloured so the change is plain on screen, with a fine diagonal hatch that only
shows when the larger picture is really the one being sampled.

    pistol icon of the HUD        gold instead of white
    Ram's shirt                   green
    the purple El Camino's body   teal

No package beyond Python 3.9 is needed.
"""
import csv
import os
import struct
import sys
import zlib

SCALE = 4
# name in the index, hue rotation in degrees, saturation gain, tint added to grey pixels, hatch strength
SAMPLES = [
    ('/data/Textures/Hud/HUD_PSP/Pistol.bmp', 0, 1.0, (70, 40, -110), 0.35),
    ('/data/textures/characters/ingame_ram/ram_shirt.bmp', 120, 1.3, (0, 0, 0), 0.12),
    ('/data/Models/Vehicles/ElCamino_LosTorosRose/textures_elcamino_lostorosrose/CaminoBodyPurple.BMP', -110, 1.2, (0, 0, 0), 0.10),
]


def read_png(path):
    """8-bit RGB, RGBA or palette PNG -> (width, height, RGBA bytes)."""
    data = open(path, 'rb').read()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise SystemExit('%s is not a PNG file' % path)
    at, parts, header, palette, alpha = 8, [], None, b'', b''
    while at < len(data):
        size, kind = struct.unpack('>I4s', data[at:at + 8])
        body = data[at + 8:at + 8 + size]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            parts.append(body)
        elif kind == b'PLTE':
            palette = body
        elif kind == b'tRNS':
            alpha = body
        at += 12 + size
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or interlace or colour not in (2, 3, 6):
        raise SystemExit('%s: only 8-bit RGB, RGBA and palette pictures are read' % path)
    channels = {2: 3, 3: 1, 6: 4}[colour]
    raw, stride = zlib.decompress(b''.join(parts)), width * channels
    rows, previous, at = [], bytearray(stride), 0
    for _ in range(height):
        kind, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b, c = previous[i], previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        previous = line
    out = bytearray(width * height * 4)
    flat = b''.join(bytes(row) for row in rows)
    if colour == 6:
        out[:] = flat
    elif colour == 2:
        out[0::4], out[1::4], out[2::4], out[3::4] = flat[0::3], flat[1::3], flat[2::3], b'\xff' * (width * height)
    else:
        for i, v in enumerate(flat):
            out[i * 4:i * 4 + 3] = palette[v * 3:v * 3 + 3]
            out[i * 4 + 3] = alpha[v] if v < len(alpha) else 255
    return width, height, bytes(out)


def write_png(path, width, height, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    stride = width * 4
    raw = b''.join(b'\x00' + rgba[y * stride:(y + 1) * stride] for y in range(height))
    with open(path, 'wb') as out:
        out.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                  chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def enlarge(width, height, rgba, scale):
    """Bilinear enlargement, texel centres kept, edges clamped; colour weighted by alpha."""
    out = bytearray(width * scale * height * scale * 4)
    for y in range(height * scale):
        fy = (y + 0.5) / scale - 0.5
        y0 = int(fy // 1)
        wy = fy - y0
        ya, yb = min(max(y0, 0), height - 1), min(max(y0 + 1, 0), height - 1)
        for x in range(width * scale):
            fx = (x + 0.5) / scale - 0.5
            x0 = int(fx // 1)
            wx = fx - x0
            xa, xb = min(max(x0, 0), width - 1), min(max(x0 + 1, 0), width - 1)
            taps = ((ya * width + xa, (1 - wx) * (1 - wy)), (ya * width + xb, wx * (1 - wy)),
                    (yb * width + xa, (1 - wx) * wy), (yb * width + xb, wx * wy))
            alpha = sum(rgba[i * 4 + 3] * w for i, w in taps)
            at = (y * width * scale + x) * 4
            for c in range(3):
                if alpha > 0:
                    out[at + c] = min(255, int(sum(rgba[i * 4 + c] * rgba[i * 4 + 3] * w for i, w in taps) / alpha + 0.5))
                else:
                    out[at + c] = min(255, int(sum(rgba[i * 4 + c] * w for i, w in taps) + 0.5))
            out[at + 3] = min(255, int(alpha + 0.5))
    return bytes(out)


def recolour(width, height, rgba, degrees, gain, tint, hatch):
    import colorsys
    out = bytearray(rgba)
    for y in range(height):
        for x in range(width):
            at = (y * width + x) * 4
            r, g, b = out[at] / 255.0, out[at + 1] / 255.0, out[at + 2] / 255.0
            h, s, v = colorsys.rgb_to_hsv(r, g, b)
            r, g, b = colorsys.hsv_to_rgb((h + degrees / 360.0) % 1.0, min(1.0, s * gain), v)
            r, g, b = (r + tint[0] / 255.0 * v, g + tint[1] / 255.0 * v, b + tint[2] / 255.0 * v)
            # Two-pixel diagonal lines: detail the game's own picture is too small to hold.
            if (x + y) % 6 < 2:
                r, g, b = r * (1.0 - hatch), g * (1.0 - hatch), b * (1.0 - hatch)
            out[at] = min(255, max(0, int(r * 255.0 + 0.5)))
            out[at + 1] = min(255, max(0, int(g * 255.0 + 0.5)))
            out[at + 2] = min(255, max(0, int(b * 255.0 + 0.5)))
    return bytes(out)


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    extracted, mods = argv[1], argv[2]
    rows = {row['name'].lower(): row for row in csv.DictReader(open(os.path.join(extracted, 'index.csv'), newline='', encoding='utf-8'))}
    folder = os.path.join(mods, 'sample_textures', 'textures')
    os.makedirs(folder, exist_ok=True)
    for name, degrees, gain, tint, hatch in SAMPLES:
        row = rows.get(name.lower())
        if row is None:
            raise SystemExit('%s is not in the index' % name)
        width, height, rgba = read_png(os.path.join(extracted, row['png'].replace('/', os.sep)))
        big = recolour(width * SCALE, height * SCALE, enlarge(width, height, rgba, SCALE), degrees, gain, tint, hatch)
        path = os.path.join(folder, row['key'] + '.png')
        write_png(path, width * SCALE, height * SCALE, big)
        print('%s  %dx%d -> %dx%d  %s' % (row['key'], width, height, width * SCALE, height * SCALE, name))
    print('mod written to', os.path.join(mods, 'sample_textures'))


if __name__ == '__main__':
    main(sys.argv)
