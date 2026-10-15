#!/usr/bin/env python3
"""Convert the game's standalone .texture_psp files to PNG and back.

  texture_psp.py info     <file.texture_psp>
  texture_psp.py to-png   <file.texture_psp> <image.png>
  texture_psp.py from-png <image.png> <original.texture_psp> <new.texture_psp>

Covers the 57 standalone textures of assets00.naz (boot screen, load screens,
concept art): 512x272 pixels, 32-bit RGBA, swizzled. The file is a short header
with the asset's name, a few GE commands that describe the texture (TMODE,
TPSM, TEXBUFW, TSIZE, ... RET), twelve zero bytes and then the pixels to the end
of the file. from-png keeps everything before the pixels from the original, so
the new image must have the original's size.

Textures inside .map and .main blocks are mostly paletted or DXT and are not
handled here.

PNG files are read and written without any extra package (8-bit RGB or RGBA,
not interlaced); with Pillow installed any PNG it can open is accepted.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import naz  # noqa: E402

BLOCK_BYTES, BLOCK_ROWS = 16, 8
PNG_MAGIC = b'\x89PNG\r\n\x1a\n'


class TextureError(Exception):
    pass


class Texture:
    """Where the pixels of a .texture_psp file are and how they are stored."""

    def __init__(self, data):
        # TFLUSH then RET closes the GE command snippet.
        end = data.find(b'\x00\x00\x00\xcb\x00\x00\x00\x0b') + 8
        if end < 8:
            raise TextureError('no GE texture commands found; not a standalone .texture_psp file')
        commands = {}
        at = end - 4
        while at >= 4:
            word = struct.unpack_from('<I', data, at)[0]
            if (word >> 24) not in (0x0B, 0xCB, 0xC2, 0xC3, 0xC6, 0xC8, 0xA0, 0xA8, 0xB8):
                break
            commands[word >> 24] = word & 0xFFFFFF
            at -= 4
        if 0xC3 not in commands or 0xA8 not in commands:
            raise TextureError('GE texture commands are incomplete')
        self.pixel_format = commands[0xC3]
        self.swizzled = bool(commands.get(0xC2, 0) & 1)
        self.width = commands[0xA8] & 0xFFFF
        self.pixel_offset = end + 12
        if self.pixel_format != 3:
            raise TextureError('pixel format %d is not 32-bit RGBA; only the 32-bit textures are supported' % self.pixel_format)
        rows, rest = divmod(len(data) - self.pixel_offset, self.width * 4) if self.width else (0, 1)
        if rest or rows <= 0 or (self.swizzled and (rows % BLOCK_ROWS or (self.width * 4) % BLOCK_BYTES)):
            raise TextureError('pixel data does not fit a %d pixel wide 32-bit image' % self.width)
        self.height = rows


def unswizzle(data, row_bytes, rows):
    out = bytearray(len(data))
    at = 0
    for top in range(0, rows, BLOCK_ROWS):
        for left in range(0, row_bytes, BLOCK_BYTES):
            for row in range(top, top + BLOCK_ROWS):
                start = row * row_bytes + left
                out[start:start + BLOCK_BYTES] = data[at:at + BLOCK_BYTES]
                at += BLOCK_BYTES
    return bytes(out)


def swizzle(data, row_bytes, rows):
    out = bytearray(len(data))
    at = 0
    for top in range(0, rows, BLOCK_ROWS):
        for left in range(0, row_bytes, BLOCK_BYTES):
            for row in range(top, top + BLOCK_ROWS):
                start = row * row_bytes + left
                out[at:at + BLOCK_BYTES] = data[start:start + BLOCK_BYTES]
                at += BLOCK_BYTES
    return bytes(out)


def decode(data):
    """Returns (width, height, RGBA bytes in row order) of a .texture_psp file."""
    texture = Texture(data)
    pixels = data[texture.pixel_offset:]
    if texture.swizzled:
        pixels = unswizzle(pixels, texture.width * 4, texture.height)
    return texture.width, texture.height, pixels


def encode(original, width, height, rgba):
    """The original file with its pixels replaced by an RGBA image of the same size."""
    texture = Texture(original)
    if (width, height) != (texture.width, texture.height):
        raise TextureError('the image is %dx%d, the texture is %dx%d' % (width, height, texture.width, texture.height))
    pixels = swizzle(rgba, width * 4, height) if texture.swizzled else bytes(rgba)
    return original[:texture.pixel_offset] + pixels


def write_png(path, width, height, rgba):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)
    rows = b''.join(b'\x00' + rgba[y * width * 4:(y + 1) * width * 4] for y in range(height))
    naz.guard_output(path)
    with open(path, 'wb') as out:
        out.write(PNG_MAGIC + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                  chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def _read_png_plain(data):
    if data[:8] != PNG_MAGIC:
        raise TextureError('not a PNG file')
    at, header, packed = 8, None, b''
    while at + 8 <= len(data):
        length, kind = struct.unpack_from('>I4s', data, at)
        body = data[at + 8:at + 8 + length]
        if kind == b'IHDR':
            header = struct.unpack('>IIBBBBB', body)
        elif kind == b'IDAT':
            packed += body
        elif kind == b'IEND':
            break
        at += 12 + length
    if header is None:
        raise TextureError('PNG has no header')
    width, height, depth, colour, _compression, _filter, interlace = header
    if depth != 8 or colour not in (2, 6) or interlace:
        raise TextureError('only 8-bit RGB or RGBA PNG files that are not interlaced can be read without Pillow')
    size = 3 if colour == 2 else 4
    stride = width * size
    raw = zlib.decompress(packed)
    if len(raw) != (stride + 1) * height:
        raise TextureError('PNG pixel data has the wrong length')
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        row = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        if kind == 1:
            for i in range(size, stride):
                row[i] = (row[i] + row[i - size]) & 0xFF
        elif kind == 2:
            for i in range(stride):
                row[i] = (row[i] + previous[i]) & 0xFF
        elif kind == 3:
            for i in range(stride):
                row[i] = (row[i] + (((row[i - size] if i >= size else 0) + previous[i]) >> 1)) & 0xFF
        elif kind == 4:
            for i in range(stride):
                left = row[i - size] if i >= size else 0
                up, corner = previous[i], previous[i - size] if i >= size else 0
                estimate = left + up - corner
                near_left, near_up, near_corner = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
                row[i] = (row[i] + (left if near_left <= near_up and near_left <= near_corner else up if near_up <= near_corner else corner)) & 0xFF
        elif kind != 0:
            raise TextureError('PNG uses an unknown row filter')
        rows.append(row)
        previous = row
    pixels = b''.join(bytes(row) for row in rows)
    if size == 3:
        rgba = bytearray(width * height * 4)
        rgba[0::4], rgba[1::4], rgba[2::4] = pixels[0::3], pixels[1::3], pixels[2::3]
        rgba[3::4] = b'\xff' * (width * height)
        pixels = bytes(rgba)
    return width, height, pixels


def read_png(path):
    """Returns (width, height, RGBA bytes)."""
    with open(path, 'rb') as stream:
        data = stream.read()
    try:
        return _read_png_plain(data)
    except TextureError:
        try:
            from PIL import Image
        except ImportError:
            raise
        image = Image.open(path).convert('RGBA')
        return image.width, image.height, image.tobytes()


def main(argv=None):
    arguments = sys.argv[1:] if argv is None else argv
    try:
        if len(arguments) == 2 and arguments[0] == 'info':
            with open(arguments[1], 'rb') as stream:
                texture = Texture(stream.read())
            print('%dx%d, 32-bit RGBA, %s, pixels at offset 0x%X' % (texture.width, texture.height,
                  'swizzled' if texture.swizzled else 'not swizzled', texture.pixel_offset))
        elif len(arguments) == 3 and arguments[0] == 'to-png':
            with open(arguments[1], 'rb') as stream:
                width, height, rgba = decode(stream.read())
            write_png(arguments[2], width, height, rgba)
            print('wrote %s (%dx%d)' % (arguments[2], width, height))
        elif len(arguments) == 4 and arguments[0] == 'from-png':
            width, height, rgba = read_png(arguments[1])
            with open(arguments[2], 'rb') as stream:
                original = stream.read()
            converted = encode(original, width, height, rgba)
            naz.guard_output(arguments[3])
            with open(arguments[3], 'wb') as out:
                out.write(converted)
            print('wrote %s' % arguments[3])
        else:
            print(__doc__.strip(), file=sys.stderr)
            return 2
    except (TextureError, naz.NazError, OSError, zlib.error) as error:
        print('texture_psp.py: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
