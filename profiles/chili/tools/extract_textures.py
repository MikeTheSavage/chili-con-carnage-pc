#!/usr/bin/env python3
"""Extract every texture of the game to PNG and compute its replacement key.

  extract_textures.py list    <assets00.naz> [word ...] [--block NAME]
  extract_textures.py extract <assets00.naz> <output folder> [--mips] [--jobs N]
  extract_textures.py one     <assets00.naz> <name or key> <image.png> [--block NAME] [--level N]
  extract_textures.py capture <trace.jsonl> [index.json]

The game's textures are in three places of assets00.naz:

  * texture assets (type 0, names ending .bmp) inside the .main and .map blocks,
  * the glyph atlas inside each font asset (type 1) of those blocks,
  * the standalone .texture_psp files (load screens, concept art).

All three hold the same structure, called gfx here (little-endian):

  +0   i32  pixel data, one of three places:
            the asset's payload, when bit 15 of the size is set (the value then points back
            at the asset record);
            inside the record, at gfx + value, when that lies inside the record;
            otherwise in the block's second segment, at offset value from its start
  +4   i32  palette, 0 when there is none: inside the record at gfx + 4 + value when that
            lies inside the record, otherwise in the second segment at offset value
  +8   i32  GE command list: gfx + 8 + value (always 8, so the list is at +16)
  +12  u16  size of the pixel data in 128-byte units; bit 15 set when it is the payload
  +14  u16  not used here
  +16  GE commands up to RET: TMODE C2 (bit 0 swizzled, bits 16-18 last mip level),
       TPSM C3 (format), TFLT C6, TLEVEL C8, then per mip level TBPn A0+n (offset of the
       level inside the pixel data), TBWn A8+n (buffer width) and TSIZEn B8+n,
       TFLUSH CB, and for paletted formats CBP B0, CBPH B1 (zero in the file) and
       CLOAD C4 (palette size in 32-byte blocks)

A texture asset points at its gfx with the self-relative pointer at record +0x28.
The second segment is the part of a block between its header and the asset records
(props.py calls it the stream); only .map blocks have one, and most of their pixel
data and palettes are there. Of the 7,274 texture assets 2,097 keep their pixels in
the payload, 1,731 in the record and 3,446 in the segment.
The palette mode is not in the file: the engine sets CMODE to C500FF03 once
(32-bit entries, shift 0, mask 255, start 0).

Pixels are decoded the way the Direct3D 12 renderer decodes them
(host/gpu/ge_renderer.cpp, decode_texture_rgba_into). The replacement key is
specified in mods/TEXTURES.md and implemented by texture_key() below.

Nothing here needs an extra package. Names are matched without regard to case.
"""
import argparse
import csv
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import naz          # noqa: E402
import props        # noqa: E402
import texture_psp  # noqa: E402

FORMAT_NAMES = {0: '5650', 1: '5551', 2: '4444', 3: '8888', 4: 'CLUT4', 5: 'CLUT8', 6: 'CLUT16', 7: 'CLUT32',
                8: 'DXT1', 9: 'DXT3', 10: 'DXT5'}
PALETTE_NAMES = {0: '5650', 1: '5551', 2: '4444', 3: '8888'}
TEXEL_BITS = {0: 16, 1: 16, 2: 16, 3: 32, 4: 4, 5: 8, 6: 16, 7: 32}
ENGINE_CMODE = 0x00FF03          # the engine's only CMODE: 8888 entries, shift 0, mask 255, start 0
TEXTURE_TYPE, FONT_TYPE = 0, 1
STANDALONE_HEADER = 0x17
KEY_MAGIC = b'CTK1'
SCREEN_ROWS = 272
INDEX_VERSION = 1


class TextureError(Exception):
    pass


# ---------------------------------------------------------------- XXH64

_M = 0xFFFFFFFFFFFFFFFF
_P1, _P2, _P3 = 0x9E3779B185EBCA87, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9
_P4, _P5 = 0x85EBCA77C2B2AE63, 0x27D4EB2F165667C5


def xxh64(data, seed=0):
    """XXH64 of a bytes-like object: the hash of host/state_hash.hpp (chili::xxh64)."""
    data = bytes(data)
    size, at = len(data), 0
    if size >= 32:
        v1, v2, v3, v4 = (seed + _P1 + _P2) & _M, (seed + _P2) & _M, seed & _M, (seed - _P1) & _M
        stripes = size // 32
        words = struct.unpack_from('<%dQ' % (stripes * 4), data, 0)
        for i in range(0, stripes * 4, 4):
            v1 = (v1 + words[i] * _P2) & _M
            v1 = (((v1 << 31) | (v1 >> 33)) * _P1) & _M
            v2 = (v2 + words[i + 1] * _P2) & _M
            v2 = (((v2 << 31) | (v2 >> 33)) * _P1) & _M
            v3 = (v3 + words[i + 2] * _P2) & _M
            v3 = (((v3 << 31) | (v3 >> 33)) * _P1) & _M
            v4 = (v4 + words[i + 3] * _P2) & _M
            v4 = (((v4 << 31) | (v4 >> 33)) * _P1) & _M
        at = stripes * 32
        h = (((v1 << 1) | (v1 >> 63)) + ((v2 << 7) | (v2 >> 57)) + ((v3 << 12) | (v3 >> 52)) + ((v4 << 18) | (v4 >> 46))) & _M
        for v in (v1, v2, v3, v4):
            v = (v * _P2) & _M
            v = (((v << 31) | (v >> 33)) * _P1) & _M
            h = ((h ^ v) * _P1 + _P4) & _M
    else:
        h = (seed + _P5) & _M
    h = (h + size) & _M
    while at + 8 <= size:
        k = (struct.unpack_from('<Q', data, at)[0] * _P2) & _M
        k = (((k << 31) | (k >> 33)) * _P1) & _M
        h ^= k
        h = ((((h << 27) | (h >> 37)) & _M) * _P1 + _P4) & _M
        at += 8
    if at + 4 <= size:
        h ^= (struct.unpack_from('<I', data, at)[0] * _P1) & _M
        h = ((((h << 23) | (h >> 41)) & _M) * _P2 + _P3) & _M
        at += 4
    while at < size:
        h ^= (data[at] * _P5) & _M
        h = ((((h << 11) | (h >> 53)) & _M) * _P1) & _M
        at += 1
    h ^= h >> 33
    h = (h * _P2) & _M
    h ^= h >> 29
    h = (h * _P3) & _M
    h ^= h >> 32
    return h


# ---------------------------------------------------------------- texel storage

def key_rows(fmt, width, height):
    """Rows of level 0 that the key and the image cover.

    A 32-bit 512x512 texture is a screen image: the engine stores 272 rows of it."""
    return SCREEN_ROWS if fmt == 3 and width == 512 and height == 512 else height


def storage_bytes(fmt, swizzled, buffer_width, height):
    """Bytes a level takes in the file (the swizzled layout is padded to 16 bytes by 8 rows)."""
    buffer_width = max(1, buffer_width)
    if fmt >= 8:
        return max(1, (buffer_width + 3) // 4) * ((height + 3) // 4) * (8 if fmt == 8 else 16)
    pitch = (buffer_width * TEXEL_BITS[fmt] + 7) // 8
    if swizzled:
        return ((pitch + 15) // 16 * 16) * ((height + 7) // 8 * 8)
    return pitch * height


def texel_bytes(data, base, fmt, swizzled, buffer_width, width, rows):
    """The texel bytes of one level in row order, without padding: T of the key.

    Formats 0-7: for each row the first ceil(width * bits / 8) bytes, read through the
    swizzle mapping when swizzled. DXT: for each row of blocks the first ceil(width / 4)
    blocks. Raises TextureError when the data ends before the last byte."""
    if fmt not in FORMAT_NAMES:
        raise TextureError('unknown texture format %d' % fmt)
    buffer_width = max(1, buffer_width)
    if fmt >= 8:
        block = 8 if fmt == 8 else 16
        per_row, used = max(1, (buffer_width + 3) // 4) * block, (width + 3) // 4 * block
        block_rows = (rows + 3) // 4
        if used == per_row:
            out = bytes(data[base:base + per_row * block_rows])
        else:
            out = b''.join(bytes(data[base + y * per_row:base + y * per_row + used]) for y in range(block_rows))
        expected = used * block_rows
    else:
        bits = TEXEL_BITS[fmt]
        pitch, used = (buffer_width * bits + 7) // 8, (width * bits + 7) // 8
        expected = used * rows
        if not swizzled:
            if used == pitch:
                out = bytes(data[base:base + pitch * rows])
            else:
                out = b''.join(bytes(data[base + y * pitch:base + y * pitch + used]) for y in range(rows))
        else:
            blocks_per_row, blocks = (pitch + 15) // 16, (used + 15) // 16
            parts = []
            for y in range(rows):
                row = base + (y >> 3) * blocks_per_row * 128 + (y & 7) * 16
                if blocks == 1:
                    parts.append(data[row:row + used])
                else:
                    parts.extend(data[row + x * 128:row + x * 128 + 16] for x in range(blocks - 1))
                    last = row + (blocks - 1) * 128
                    parts.append(data[last:last + used - (blocks - 1) * 16])
            out = b''.join(bytes(part) for part in parts)
        if fmt == 4 and width & 1:
            # The high nibble of a row's last byte is not a texel.
            out = bytearray(out)
            for at in range(used - 1, len(out), used):
                out[at] &= 0x0F
            out = bytes(out)
    if len(out) != expected:
        raise TextureError('the pixel data ends before the texture does (%d of %d bytes)' % (len(out), expected))
    return out


_LOW = bytes(v & 15 for v in range(256))
_HIGH = bytes(v >> 4 for v in range(256))


def texel_indices(texels, fmt, width, rows):
    """Palette index of every texel, in row order (a bytes object for CLUT4 and CLUT8, a tuple otherwise)."""
    if fmt == 5:
        return texels
    if fmt == 4:
        used = (width + 1) // 2
        out = bytearray(len(texels) * 2)
        out[0::2] = texels.translate(_LOW)
        out[1::2] = texels.translate(_HIGH)
        if width & 1:
            out = b''.join(bytes(out[y * used * 2:y * used * 2 + width]) for y in range(rows))
        return bytes(out)
    if fmt == 6:
        return struct.unpack('<%dH' % (len(texels) // 2), texels)
    if fmt == 7:
        return struct.unpack('<%dI' % (len(texels) // 4), texels)
    raise TextureError('format %d has no palette' % fmt)


def palette_entry(index, cmode):
    """Entry number a raw index selects (ge_renderer.cpp read_clut_fast)."""
    wrap = 0xFF if cmode & 3 == 3 else 0x1FF
    return ((((index >> ((cmode >> 2) & 0x1F)) & ((cmode >> 8) & 0xFF)) | (((cmode >> 16) & 0x1F) << 4)) & wrap)


def used_palette(indices, palette, cmode):
    """P of the key: the entries of the index values that occur, in ascending index order."""
    size = 4 if cmode & 3 == 3 else 2
    parts = []
    for index in sorted(set(indices)):
        at = palette_entry(index, cmode) * size
        if at + size > len(palette):
            raise TextureError('the texture uses palette entry %d, the palette has %d' % (at // size, len(palette) // size))
        parts.append(bytes(palette[at:at + size]))
    return b''.join(parts)


def texture_key(fmt, width, height, texels, cmode=ENGINE_CMODE, palette=b''):
    """The replacement key (mods/TEXTURES.md) of a level given its texel bytes T and the palette memory.

    width and height are the TSIZE values the game binds; texels must hold key_rows() rows."""
    rows = key_rows(fmt, width, height)
    paletted = 4 <= fmt <= 7
    header = KEY_MAGIC + bytes((fmt, cmode & 3 if paletted else 0, 0, 0)) + struct.pack('<HHI', width, height, 0)
    used = used_palette(texel_indices(texels, fmt, width, rows), palette, cmode) if paletted else b''
    return xxh64(used, xxh64(texels, xxh64(header)))


# ---------------------------------------------------------------- decoding (ge_renderer.cpp)

def _expand4(v):
    return ((v & 15) << 4) | (v & 15)


def _expand5(v):
    v &= 31
    return (v << 3) | (v >> 2)


def _expand6(v):
    v &= 63
    return (v << 2) | (v >> 4)


def unpack16(pixel, fmt):
    if fmt == 0:
        return _expand5(pixel), _expand6(pixel >> 5), _expand5(pixel >> 11), 255
    if fmt == 1:
        return _expand5(pixel), _expand5(pixel >> 5), _expand5(pixel >> 10), 255 if pixel & 0x8000 else 0
    if fmt == 2:
        return _expand4(pixel), _expand4(pixel >> 4), _expand4(pixel >> 8), _expand4(pixel >> 12)
    return 0, 0, 0, 0


def palette_colour(palette, entry, palette_format):
    """RGBA of one palette entry; (0, 0, 0, 0) past the end, as for memory the renderer cannot read."""
    if palette_format == 3:
        colour = palette[entry * 4:entry * 4 + 4]
        return tuple(colour) if len(colour) == 4 else (0, 0, 0, 0)
    raw = palette[entry * 2:entry * 2 + 2]
    return unpack16(raw[0] | (raw[1] << 8), palette_format) if len(raw) == 2 else (0, 0, 0, 0)


def _dxt_endpoint(c):
    return (c >> 8) & 0xF8, (c >> 3) & 0xFC, (c << 3) & 0xF8


def _dxt_colours(c1, c2, dxt1):
    a, b = _dxt_endpoint(c1), _dxt_endpoint(c2)
    if c1 > c2 or not dxt1:
        third = tuple((a[i] * 2 + b[i]) // 3 for i in range(3))
        fourth = tuple((b[i] * 2 + a[i]) // 3 for i in range(3))
        return a, b, third, fourth, False
    return a, b, tuple((a[i] + b[i]) // 2 for i in range(3)), (0, 0, 0), True


def _dxt5_alphas(a1, a2):
    out = [a1, a2, 0, 0, 0, 0, 0, 255]
    if a1 > a2:
        for i in range(1, 7):
            out[i + 1] = ((a1 * ((7 - i) << 8)) // 7 + (a2 * (i << 8)) // 7 + 31) >> 8
    else:
        for i in range(1, 5):
            out[i + 1] = ((a1 * ((5 - i) << 8)) // 5 + (a2 * (i << 8)) // 5 + 31) >> 8
    return out


def _decode_dxt(texels, fmt, width, rows):
    size = 8 if fmt == 8 else 16
    across, down = (width + 3) // 4, (rows + 3) // 4
    lines = [[] for _ in range(down * 4)]
    for by in range(down):
        for bx in range(across):
            at = (by * across + bx) * size
            c1, c2 = struct.unpack_from('<HH', texels, at + 4)
            a, b, c, d, punch = _dxt_colours(c1, c2, fmt == 8)
            if fmt == 8:
                colours = (bytes(a + (255,)), bytes(b + (255,)), bytes(c + (255,)), bytes(d + (0 if punch else 255,)))
                for y in range(4):
                    s = texels[at + y]
                    lines[by * 4 + y].append(colours[s & 3] + colours[(s >> 2) & 3] + colours[(s >> 4) & 3] + colours[s >> 6])
                continue
            colours = (bytes(a), bytes(b), bytes(c), bytes(d))
            if fmt == 9:
                for y in range(4):
                    s = texels[at + y]
                    alpha = texels[at + 8 + y * 2] | (texels[at + 9 + y * 2] << 8)
                    lines[by * 4 + y].append(b''.join(
                        colours[(s >> (x * 2)) & 3] + bytes((((alpha >> (x * 4)) & 15) * 17,)) for x in range(4)))
            else:
                alphas = _dxt5_alphas(texels[at + 14], texels[at + 15])
                selectors = int.from_bytes(texels[at + 8:at + 14], 'little')
                for y in range(4):
                    s = texels[at + y]
                    lines[by * 4 + y].append(b''.join(
                        colours[(s >> (x * 2)) & 3] + bytes((alphas[(selectors >> (y * 12 + x * 3)) & 7],))
                        for x in range(4)))
    return b''.join(b''.join(line)[:width * 4] for line in lines[:rows])


def decode_level(texels, fmt, width, rows, palette=b'', cmode=ENGINE_CMODE):
    """RGBA bytes (width x rows) of a level from its texel bytes, as the Direct3D 12 renderer decodes them."""
    if fmt == 3:
        return bytes(texels)
    if fmt in (0, 1, 2):
        out = bytearray(width * rows * 4)
        for i, pixel in enumerate(struct.unpack('<%dH' % (width * rows), texels)):
            out[i * 4:i * 4 + 4] = bytes(unpack16(pixel, fmt))
        return bytes(out)
    if fmt >= 8:
        return _decode_dxt(texels, fmt, width, rows)
    indices = texel_indices(texels, fmt, width, rows)
    palette_format = cmode & 3
    out = bytearray(width * rows * 4)
    if fmt in (4, 5):
        colours = [palette_colour(palette, palette_entry(v, cmode), palette_format) for v in range(256)]
        for channel in range(4):
            out[channel::4] = indices.translate(bytes(colour[channel] for colour in colours))
        return bytes(out)
    cache = {}
    for i, index in enumerate(indices):
        colour = cache.get(index)
        if colour is None:
            colour = cache[index] = bytes(palette_colour(palette, palette_entry(index, cmode), palette_format))
        out[i * 4:i * 4 + 4] = colour
    return bytes(out)


# ---------------------------------------------------------------- reading the archive

class Level:
    __slots__ = ('offset', 'buffer_width', 'width', 'height')

    def __init__(self, offset, buffer_width, width, height):
        self.offset, self.buffer_width, self.width, self.height = offset, buffer_width, width, height


class Texture:
    """One texture as stored: its GE state, its pixel data and its palette."""

    def __init__(self, source, container, name):
        self.source, self.container, self.name = source, container, name   # 'block', 'font' or 'file'
        self.format = self.tmode = self.tflt = self.tlevel = 0
        self.swizzled = False
        self.levels = []
        self.pixels = b''
        self.palette = b''
        self.declared_levels = 1
        self.in_payload = False
        self.place = ''             # where the pixels are: 'payload', 'record' or 'segment'

    @property
    def width(self):
        return self.levels[0].width

    @property
    def height(self):
        """Rows of the image: the declared height, or 272 for a screen image."""
        return key_rows(self.format, self.levels[0].width, self.levels[0].height)

    def level_size(self, number):
        level = self.levels[number]
        return level.width, (self.height if number == 0 else level.height)

    def texels(self, number=0):
        level = self.levels[number]
        width, rows = self.level_size(number)
        return texel_bytes(self.pixels, level.offset, self.format, self.swizzled, level.buffer_width, width, rows)

    def key(self, number=0):
        level = self.levels[number]
        return texture_key(self.format, level.width, level.height, self.texels(number), ENGINE_CMODE, self.palette)

    def rgba(self, number=0):
        width, rows = self.level_size(number)
        return width, rows, decode_level(self.texels(number), self.format, width, rows, self.palette, ENGINE_CMODE)

    def fingerprint(self):
        """Equal for two records that store the same texture."""
        state = struct.pack('<4I', self.format, self.tmode, len(self.levels), len(self.palette))
        state += b''.join(struct.pack('<4I', l.offset, l.buffer_width, l.width, l.height) for l in self.levels)
        return hashlib.blake2b(state + self.palette + self.pixels, digest_size=16).digest()


def read_gfx(texture, memory, gfx, record, limit, payload=b'', segment=b''):
    """Fills a Texture from the gfx structure at memory[gfx].

    memory is a block's info buffer or a whole .texture_psp file, record..limit the asset record
    around the structure, payload the asset's own payload bytes, segment the block's second segment.
    Pixels are the payload (bit 15 of the size), or inside the record (self-relative pointer), or in
    the segment (the pointer is then an offset from the segment's start); the palette is inside the
    record or in the segment."""
    if gfx < record or gfx + 16 > limit:
        raise TextureError('the texture structure lies outside its record')
    pixels, palette, commands_at, units = struct.unpack_from('<iiiH', memory, gfx)
    at, commands = gfx + 8 + commands_at, {}
    if commands_at < 8:
        raise TextureError('bad GE list pointer')
    for _ in range(64):
        if at + 4 > limit:
            raise TextureError('the GE list runs past the record')
        word = struct.unpack_from('<I', memory, at)[0]
        at += 4
        if word >> 24 == 0x0B:
            break
        if word != 0xFFFFFFFF:
            commands[word >> 24] = word & 0xFFFFFF
    else:
        raise TextureError('the GE list has no RET')
    for needed in (0xC2, 0xC3, 0xA0, 0xA8, 0xB8):
        if needed not in commands:
            raise TextureError('the GE list has no command %02X' % needed)
    texture.tmode, texture.tflt, texture.tlevel = commands[0xC2], commands.get(0xC6, 0), commands.get(0xC8, 0)
    texture.format, texture.swizzled = commands[0xC3] & 0xF, bool(commands[0xC2] & 1)
    if texture.format not in FORMAT_NAMES:
        raise TextureError('unknown texture format %d' % texture.format)
    last = (commands[0xC2] >> 16) & 7
    texture.declared_levels = last + 1
    for number in range(last + 1):
        if 0xA0 + number not in commands or 0xA8 + number not in commands or 0xB8 + number not in commands:
            break
        offset = (commands[0xA0 + number] & 0xFFFFF0) | ((commands[0xA8 + number] << 8) & 0x0F000000)
        buffer_width = commands[0xA8 + number] & 0x7FF
        size = commands[0xB8 + number]
        level = Level(offset, buffer_width, 1 << (size & 15), 1 << ((size >> 8) & 15))
        if number:
            # The renderer ends the chain at the last level that halves (ge_renderer.cpp, "stale size/address").
            before = texture.levels[-1]
            if (level.width != max(1, before.width >> 1) or level.height != max(1, before.height >> 1)
                    or offset == 0 or buffer_width == 0):
                break
        texture.levels.append(level)
    size = (units & 0x7FFF) * 128
    texture.in_payload = bool(units & 0x8000)
    if texture.in_payload:
        if size > len(payload):
            raise TextureError('the payload has %d bytes, the texture needs %d' % (len(payload), size))
        texture.pixels, texture.place = bytes(payload[:size]), 'payload'
    elif at <= gfx + pixels and gfx + pixels + size <= limit:
        texture.pixels, texture.place = bytes(memory[gfx + pixels:gfx + pixels + size]), 'record'
    elif 0 <= pixels and pixels + size <= len(segment):
        texture.pixels, texture.place = bytes(segment[pixels:pixels + size]), 'segment'
    else:
        raise TextureError('the pixel data lies outside the record and the segment')
    if 4 <= texture.format <= 7:
        if not palette:
            raise TextureError('a paletted texture without a palette')
        entries = (commands.get(0xC4, 0) & 0x3F) * 32
        start = gfx + 4 + palette
        if not entries:
            raise TextureError('a paletted texture without a CLOAD command')
        if at <= start and start + entries <= limit:
            texture.palette = bytes(memory[start:start + entries])
        elif 0 <= palette and palette + entries <= len(segment):
            texture.palette = bytes(segment[palette:palette + entries])
        else:
            raise TextureError('the palette lies outside the record and the segment')
    return texture


_RECORD = re.compile(rb'(?=[\x00-\x0c]\x00{3}.{4}\x00{4}.{4}\x00{8}\x00{2}.{2}[\x18\x19]\x00{3})', re.S)
_SNIPPET = re.compile(rb'[\x00\x01][\x00\x01][\x00-\x07]\xC2[\x00-\x0A]\x00\x00\xC3..\x00\xC6', re.S)


class Record:
    """An asset record: start..limit inside Block.memory, payload..payload+size inside the file."""
    __slots__ = ('type', 'name', 'start', 'limit', 'payload', 'size')

    def __init__(self, kind, name, start, limit, payload, size):
        self.type, self.name, self.start, self.limit, self.payload, self.size = kind, name, start, limit, payload, size


class Block:
    """A .main or .map block: its asset records, its info buffer and its second segment.

    The file is: header, a second segment (props.py calls it the stream), run-length words, the
    info buffer with the asset records, the size table, the payloads. A .map keeps most pixel
    data and palettes in the second segment.

    Raises props.BlockError unless the records found are the number the header gives and the
    sizes add up to the file (the walk of props.read_block, which keeps no offsets)."""

    def __init__(self, data, is_map):
        base = props.MAP_BASE if is_map else 0
        if len(data) < base + 37:
            raise props.BlockError('too small to be a block')
        count, info_size = struct.unpack_from('<II', data, base + 16)
        segment_start, segment_end = struct.unpack_from('<II', data, base + 29)
        if segment_end < segment_start:
            raise props.BlockError('bad stream range')
        segment = base + 37
        at, total = segment + (segment_end - segment_start), 0
        while total < count:
            if at + 4 > len(data):
                raise props.BlockError('the run-length words run past the file')
            total += struct.unpack_from('<I', data, at)[0] & 0x7FFFFFFF
            at += 4
        info, sizes_at = at, at + info_size
        payload = sizes_at + 4 * count
        if total != count or payload > len(data):
            raise props.BlockError('the header does not describe this file')
        sizes = struct.unpack_from('<%dI' % count, data, sizes_at)
        if payload + sum(sizes) != len(data):
            raise props.BlockError('the asset sizes do not add up to the file')
        found = []
        for match in _RECORD.finditer(data, info, sizes_at):
            at = match.start()
            if (at - info) & 3 or at > sizes_at - 32:
                continue
            name_at = at + 4 + (struct.unpack_from('<I', data, at + 4)[0] & ~3)
            if name_at >= sizes_at or data[name_at:name_at + 1] != b'/':
                continue
            end = data.find(b'\0', name_at, min(sizes_at, name_at + 512))
            if end < 0 or end - name_at < 5 or not all(32 <= c < 127 for c in data[name_at:end]):
                continue
            found.append((at, struct.unpack_from('<I', data, at)[0], data[name_at:end].decode('ascii')))
        if len(found) != count:
            raise props.BlockError('found %d asset records, the header says %d' % (len(found), count))
        self.data, self.info, self.info_size = data, info, info_size
        self.memory = data[info:sizes_at]
        self.segment = data[segment:segment + (segment_end - segment_start)]
        self.records = []
        for number, ((start, kind, name), size) in enumerate(zip(found, sizes)):
            limit = found[number + 1][0] if number + 1 < count else sizes_at
            self.records.append(Record(kind, name, start - info, limit - info, payload, size))
            payload += size

    def textures(self, container, problems):
        """(textures, file offsets of their GE lists). A record that cannot be read goes to problems."""
        out, claimed = [], set()
        for record in self.records:
            payload = self.data[record.payload:record.payload + record.size]
            try:
                if record.type == TEXTURE_TYPE:
                    gfx = record.start + 0x28 + struct.unpack_from('<I', self.memory, record.start + 0x28)[0]
                    claimed.add(self.info + gfx + 16)
                    out.append(read_gfx(Texture('block', container, record.name), self.memory, gfx,
                                        record.start, record.limit, payload, self.segment))
                elif record.type == FONT_TYPE:
                    # A font keeps its glyph atlas as one gfx structure inside its record.
                    for match in _SNIPPET.finditer(self.memory, record.start, record.limit):
                        gfx = match.start() - 16
                        if gfx < record.start or struct.unpack_from('<i', self.memory, gfx + 8)[0] != 8:
                            continue
                        claimed.add(self.info + match.start())
                        out.append(read_gfx(Texture('font', container, record.name), self.memory, gfx,
                                            record.start, record.limit, payload, self.segment))
            except (TextureError, struct.error) as error:
                problems.append((container, record.name, str(error)))
        return out, claimed


def standalone_texture(data, container):
    if len(data) < STANDALONE_HEADER + 0x30:
        raise TextureError('too small to be a .texture_psp file')
    _stamp, info_size, _payload = struct.unpack_from('<III', data, 0)
    record = STANDALONE_HEADER
    if record + info_size != len(data) or struct.unpack_from('<I', data, record)[0] != TEXTURE_TYPE:
        raise TextureError('not a single texture asset')
    name_at = record + 4 + struct.unpack_from('<I', data, record + 4)[0]
    end = data.find(b'\0', name_at, name_at + 512)
    if end < 0:
        raise TextureError('the asset has no name')
    gfx = record + 0x28 + struct.unpack_from('<I', data, record + 0x28)[0]
    return read_gfx(Texture('file', container, data[name_at:end].decode('latin-1')), data, gfx, record, len(data))


def container_name(entry_name):
    """What the index calls a block or file: a block's file name, a standalone file's whole entry name."""
    return entry_name.rsplit('/', 1)[-1] if entry_name.lower().endswith(('.main', '.map')) else entry_name


def archive_textures(path, wanted_block=None, problems=None, census=None):
    """Yields every Texture of the archive in archive order.

    census, when given, collects the reconciliation: blocks, asset records, texture records and the raw
    GE-list heads found outside the textures."""
    problems = [] if problems is None else problems
    archive = props.Archive(path)
    if wanted_block and not any(naz.normal_name(wanted_block) in (naz.normal_name(entry.name), container_name(entry.name.lower()))
                                for entry in archive.entries):
        raise TextureError('the archive has no block or file %s' % wanted_block)
    for entry in archive.entries:
        low = entry.name.lower()
        is_block = low.endswith(('.main', '.map'))
        if wanted_block and naz.normal_name(wanted_block) not in (naz.normal_name(entry.name), container_name(low)):
            continue
        if not is_block and not low.endswith('.texture_psp') and census is None:
            continue
        data = archive.read(entry)
        claimed, textures = set(), []
        if is_block:
            try:
                block = Block(data, low.endswith('.map'))
                records = block.records
                textures, claimed = block.textures(container_name(entry.name), problems)
            except props.BlockError as error:
                problems.append((entry.name, '', 'block not readable: %s' % error))
                records = []
            if census is not None:
                census['blocks'] += 1
                census['asset records'] += len(records)
                census['texture records'] += sum(1 for record in records if record.type == TEXTURE_TYPE)
                census['font records'] += sum(1 for record in records if record.type == FONT_TYPE)
        elif low.endswith('.texture_psp'):
            try:
                textures = [standalone_texture(data, entry.name)]
                claimed = {m.start() for m in _SNIPPET.finditer(data)}
            except (TextureError, struct.error) as error:
                problems.append((entry.name, '', str(error)))
            if census is not None:
                census['standalone files'] += 1
        if census is not None:
            census['archive entries'] += 1
            for match in _SNIPPET.finditer(data):
                if match.start() not in claimed:
                    census['stray GE list heads'] += 1
                    census.setdefault('stray', []).append('%s at %d' % (entry.name, match.start()))
        for texture in textures:
            yield texture


# ---------------------------------------------------------------- index and output

def image_path(name):
    """Where a texture's PNG goes below the output folder: its asset path in lower case."""
    parts = [part for part in name.replace('\\', '/').lower().split('/') if part]
    if not parts or any(part in ('.', '..') for part in parts):
        raise TextureError('unsafe asset name %r' % name)
    parts = [re.sub(r'[<>:"|?*\x00-\x1f]', '_', part) for part in parts]
    base = parts[-1]
    if base.endswith('.bmp'):
        base = base[:-4]
    return '/'.join(parts[:-1] + [base])


class Entry:
    """One distinct key: everything the index says about it."""

    def __init__(self, key, texture):
        self.key, self.texture = key, texture
        self.names, self.containers, self.sources = [], [], []
        self.level_keys = []
        self.error = ''
        self.pngs = []      # one path per name, the first name's first
        self.stored = 0     # how many times the archive stores it

    def add(self, texture):
        self.stored += 1
        if texture.name.lower() not in (name.lower() for name in self.names):
            self.names.append(texture.name)
        if texture.container not in self.containers:
            self.containers.append(texture.container)
        if texture.source not in self.sources:
            self.sources.append(texture.source)


def build_index(path, wanted_block=None, level_keys=False, progress=None):
    """Reads the archive and returns (entries by key in first-seen order, occurrences, problems, census).

    Each stored texture is keyed once: records that store the same bytes share the work."""
    problems, census = [], {'archive entries': 0, 'blocks': 0, 'asset records': 0, 'texture records': 0,
                            'font records': 0, 'standalone files': 0, 'stray GE list heads': 0}
    entries, by_fingerprint, occurrences = {}, {}, 0
    stored = census['stored'] = {'by_format': {}, 'by_source': {}, 'pixels': 0}
    for texture in archive_textures(path, wanted_block, problems, census if not wanted_block else None):
        occurrences += 1
        for group, name in (('by_format', FORMAT_NAMES[texture.format]), ('by_source', texture.source)):
            stored[group][name] = stored[group].get(name, 0) + 1
        stored['pixels'] += texture.width * texture.height
        fingerprint = texture.fingerprint()
        entry = by_fingerprint.get(fingerprint)
        if entry is None:
            try:
                key = '%016x' % texture.key()
            except (TextureError, struct.error) as error:
                problems.append((texture.container, texture.name, str(error)))
                continue
            entry = entries.get(key)
            if entry is None:
                entry = entries[key] = Entry(key, texture)
                if level_keys:
                    for number in range(len(texture.levels)):
                        try:
                            entry.level_keys.append('%016x' % texture.key(number))
                        except (TextureError, struct.error) as error:
                            entry.level_keys.append('')
                            entry.error = 'mip level %d: %s' % (number, error)
            by_fingerprint[fingerprint] = entry
            if progress and len(by_fingerprint) % 500 == 0:
                progress('%d records read, %d distinct textures' % (occurrences, len(entries)))
        entry.add(texture)
    return entries, occurrences, problems, census


def assign_images(entries):
    """Gives every entry one PNG path per name. A path several keys claim gets the key in the file name."""
    claims = {}
    for entry in entries.values():
        for name in entry.names:
            claimants = claims.setdefault(image_path(name), [])
            if entry not in claimants:
                claimants.append(entry)
    for entry in entries.values():
        entry.pngs = []
        for name in entry.names:
            path = image_path(name)
            png = '%s.png' % path if len(claims[path]) == 1 else '%s.%s.png' % (path, entry.key)
            if png not in entry.pngs:
                entry.pngs.append(png)


def entry_record(entry):
    texture = entry.texture
    record = {
        'key': entry.key,
        'name': entry.names[0],
        'names': entry.names,
        'blocks': entry.containers,
        'stored': entry.stored,
        'source': '+'.join(entry.sources),
        'format': FORMAT_NAMES[texture.format],
        'palette_format': PALETTE_NAMES[ENGINE_CMODE & 3] if 4 <= texture.format <= 7 else '',
        'width': texture.width,
        'height': texture.height,
        'declared_height': texture.levels[0].height,
        'mip_levels': len(texture.levels),
        'bytes': len(texture.pixels),
        'palette_bytes': len(texture.palette),
        'swizzled': texture.swizzled,
        'stored_in': texture.place,
        'png': entry.pngs[0] if entry.pngs else '',
        'pngs': entry.pngs,
        'decoded': not entry.error,
        'error': entry.error,
    }
    if entry.level_keys:
        record['level_keys'] = entry.level_keys
    return record


CSV_COLUMNS = ('key', 'name', 'blocks', 'source', 'format', 'palette_format', 'width', 'height', 'mip_levels',
               'bytes', 'png', 'decoded', 'error', 'other_names')


def write_index(folder, archive, entries, occurrences, problems, census):
    records = [entry_record(entry) for entry in entries.values()]
    by_format, by_source = {}, {}
    for record in records:
        by_format[record['format']] = by_format.get(record['format'], 0) + 1
        by_source[record['source']] = by_source.get(record['source'], 0) + 1
    summary = {
        'stored_textures': occurrences,
        'distinct_keys': len(records),
        'decoded': sum(1 for record in records if record['decoded']),
        'failed': sum(1 for record in records if not record['decoded']) + len(problems),
        'pixels': sum(record['width'] * record['height'] for record in records),
        'by_format': dict(sorted(by_format.items())),
        'by_source': dict(sorted(by_source.items())),
        'stored_by_format': dict(sorted(census['stored']['by_format'].items())),
        'stored_by_source': dict(sorted(census['stored']['by_source'].items())),
        'stored_pixels': census['stored']['pixels'],
        'census': {name: value for name, value in census.items() if name not in ('stray', 'stored')},
        'stray_ge_list_heads': census.get('stray', []),
        'unreadable': [{'block': block, 'name': name, 'error': error} for block, name, error in problems],
    }
    target = os.path.join(folder, 'index.json')
    naz.guard_output(target)
    with open(target, 'w', encoding='utf-8', newline='\n') as out:
        json.dump({'format': 'chili-texture-index', 'version': INDEX_VERSION, 'key': KEY_MAGIC.decode('ascii'),
                   'archive': os.path.basename(archive), 'summary': summary, 'textures': records}, out, indent=1)
        out.write('\n')
    target = os.path.join(folder, 'index.csv')
    naz.guard_output(target)
    with open(target, 'w', encoding='utf-8', newline='') as out:
        writer = csv.writer(out)
        writer.writerow(CSV_COLUMNS)
        for record in records:
            writer.writerow([record['key'], record['name'], ';'.join(record['blocks']), record['source'],
                             record['format'], record['palette_format'], record['width'], record['height'],
                             record['mip_levels'], record['bytes'], record['png'],
                             'yes' if record['decoded'] else 'no', record['error'], ';'.join(record['names'][1:])])
    return summary


def write_images(job):
    """Decodes one texture and writes its PNG files. Returns (key, error text)."""
    folder, key, pngs, texture, mips = job
    number = 0
    try:
        for number in range(len(texture.levels) if mips else 1):
            width, rows, rgba = texture.rgba(number)
            for png in pngs:
                target = os.path.join(folder, *png.split('/'))
                if number:
                    target = '%s.mip%d.png' % (target[:-4], number)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                texture_psp.write_png(target, width, rows, rgba)
    except (TextureError, naz.NazError, OSError, struct.error) as error:
        return key, ('mip level %d: %s' % (number, error)) if number else str(error)
    return key, ''


def command_extract(arguments):
    folder = os.path.abspath(arguments.output)
    naz.guard_output(os.path.join(folder, 'index.json'))
    os.makedirs(folder, exist_ok=True)

    def progress(text):
        print(text, file=sys.stderr, flush=True)

    entries, occurrences, problems, census = build_index(arguments.archive, level_keys=True, progress=progress)
    assign_images(entries)
    jobs = [(folder, entry.key, entry.pngs, entry.texture, arguments.mips) for entry in entries.values()]
    if arguments.jobs > 1:
        import multiprocessing
        with multiprocessing.Pool(min(arguments.jobs, 3)) as pool:
            results = pool.imap_unordered(write_images, jobs, chunksize=16)
            results = list(results)
    else:
        results = [write_images(job) for job in jobs]
    for key, error in results:
        if error:
            entries[key].error = error
    summary = write_index(folder, arguments.archive, entries, occurrences, problems, census)
    print('%d textures stored in the archive, %d distinct keys' % (summary['stored_textures'], summary['distinct_keys']))
    print('decoded %d, failed %d, %d pixels' % (summary['decoded'], summary['failed'], summary['pixels']))
    print('distinct by format: %s' % ', '.join('%s %d' % item for item in summary['by_format'].items()))
    print('distinct by source: %s' % ', '.join('%s %d' % item for item in summary['by_source'].items()))
    print('stored by format: %s' % ', '.join('%s %d' % item for item in summary['stored_by_format'].items()))
    print('stored by source: %s' % ', '.join('%s %d' % item for item in summary['stored_by_source'].items()))
    print('stored pixels: %d' % summary['stored_pixels'])
    print('census: %s' % ', '.join('%s %d' % item for item in summary['census'].items()))
    for entry in entries.values():
        if entry.error:
            print('FAILED %s %s: %s' % (entry.key, entry.names[0], entry.error))
    for block, name, error in problems:
        print('UNREADABLE %s %s: %s' % (block, name, error))
    print('wrote %s' % folder)


def _matches(entry, words):
    text = ' '.join([entry.key] + entry.names).lower()
    return all(word.lower() in text for word in words)


def command_list(arguments):
    entries, occurrences, problems, _census = build_index(arguments.archive, arguments.block)
    shown = 0
    for entry in entries.values():
        if not _matches(entry, arguments.words):
            continue
        shown += 1
        texture = entry.texture
        print('%s  %-6s %4dx%-4d %d mip  %7d bytes  %3d blocks  %s' % (
            entry.key, FORMAT_NAMES[texture.format], texture.width, texture.height, len(texture.levels),
            len(texture.pixels), len(entry.containers), entry.names[0]))
    for block, name, error in problems:
        print('not readable: %s %s: %s' % (block, name, error), file=sys.stderr)
    print('%d of %d distinct textures (%d stored)' % (shown, len(entries), occurrences))


def command_one(arguments):
    entries, _occurrences, _problems, _census = build_index(arguments.archive, arguments.block)
    wanted = arguments.texture.lower()
    found = [entry for entry in entries.values() if entry.key == wanted]
    if not found:
        # A shell that rewrites /data/... into a Windows path still ends with the asset path.
        wanted = wanted.replace('\\', '/')
        found = [entry for entry in entries.values()
                 if any(name.lower() == wanted or name.lower().endswith('/' + wanted)
                        or name.lower().endswith('/' + wanted + '.bmp') or wanted.endswith(name.lower())
                        for name in entry.names)]
    if not found:
        raise TextureError('no texture with the key or name %s; "list" shows them' % arguments.texture)
    if len(found) > 1:
        raise TextureError('%d textures have that name (%s); give a key or --block' % (
            len(found), ', '.join(entry.key for entry in found)))
    texture = found[0].texture
    if not 0 <= arguments.level < len(texture.levels):
        raise TextureError('the texture has %d mip levels' % len(texture.levels))
    width, rows, rgba = texture.rgba(arguments.level)
    texture_psp.write_png(arguments.image, width, rows, rgba)
    print('wrote %s (%s, %dx%d, key %s)' % (arguments.image, FORMAT_NAMES[texture.format], width, rows, found[0].key))


# ---------------------------------------------------------------- keys from a GE capture

_TRACE_FORMATS = {'5650': 0, '5551': 1, '4444': 2, '8888': 3, 'clut4': 4, 'clut8': 5, 'clut16': 6, 'clut32': 7,
                  'dxt1': 8, 'dxt3': 9, 'dxt5': 10}


class Capture:
    """Guest memory of a .gecap file (host/ge_frame_capture.hpp): main RAM and VRAM as the list was queued."""

    def __init__(self, path):
        with open(path, 'rb') as stream:
            head = stream.read(16)
            if head[:8] != b'CHILIGE1':
                raise TextureError('%s is not a GE capture' % path)
            slots = struct.unpack_from('<I', head, 12)[0]
            table = stream.read(24 * slots)
            self.ram = self.vram = b''
            for number in range(slots):
                tag, offset, size = struct.unpack_from('<8sQQ', table, number * 24)
                tag = tag.rstrip(b'\0')
                if tag in (b'RAM', b'VRAM'):
                    stream.seek(offset)
                    data = stream.read(size)
                    if tag == b'RAM':
                        self.ram = data
                    else:
                        self.vram = data

    def region(self, address):
        """(bytes, offset) of a guest address, or raises TextureError."""
        address &= 0x3FFFFFFF
        if 0x08000000 <= address < 0x08000000 + len(self.ram):
            return self.ram, address - 0x08000000
        if 0x04000000 <= address < 0x04800000 and len(self.vram):
            return self.vram, (address - 0x04000000) % len(self.vram)
        raise TextureError('address %08X is not in the capture' % address)


def command_capture(arguments):
    """Computes the key of every texture the traced draws bind, from the captured guest memory."""
    known = {}
    if arguments.index:
        with open(arguments.index, encoding='utf-8') as stream:
            for record in json.load(stream)['textures']:
                known[record['key']] = record
                for number, key in enumerate(record.get('level_keys', [])[1:], 1):
                    known.setdefault(key, dict(record, name='%s (mip level %d)' % (record['name'], number)))
    folder = os.path.dirname(os.path.abspath(arguments.trace))
    with open(arguments.trace, encoding='utf-8') as stream:
        events = [json.loads(line) for line in stream if line.strip()]
    # Surfaces the GE draws into change every frame: they have no key.
    targets = set()
    for event in events:
        if event.get('ev') == 'draw':
            targets.update(int(event[name], 16) & 0x3FFFFFFF for name in ('fb', 'zb') if name in event)
    capture, bound, skipped = None, {}, {}
    for event in events:
        if event.get('ev') == 'list':
            names = [name for name in os.listdir(folder)
                     if name.startswith('ge_%06d_' % event['list']) and name.endswith('.gecap')]
            capture = Capture(os.path.join(folder, names[0])) if names else None
            continue
        if event.get('ev') != 'draw' or capture is None or not event['tex'].get('on'):
            continue
        tex = event['tex']
        fmt, address = _TRACE_FORMATS[tex['format']], int(tex['addr'], 16)
        identity = (address, fmt, tex['w'], tex['h'], tex['stride'], tex['swizzle'], event.get('clut', {}).get('addr'))
        if identity in bound or identity in skipped:
            continue
        if address & 0x3FFFFFFF in targets:
            skipped[identity] = 'a render target'
            continue
        try:
            data, base = capture.region(address)
            rows = key_rows(fmt, tex['w'], tex['h'])
            texels = texel_bytes(data, base, fmt, bool(tex['swizzle']), tex['stride'], tex['w'], rows)
            cmode, palette = ENGINE_CMODE, b''
            if 4 <= fmt <= 7:
                clut = event['clut']
                cmode = (_TRACE_FORMATS[clut['format']] | (clut['shift'] << 2) | (clut['mask'] << 8)
                         | ((clut['start'] >> 4) << 16))
                memory, at = capture.region(int(clut['addr'], 16))
                palette = memory[at:at + 1024]
            bound[identity] = '%016x' % texture_key(fmt, tex['w'], tex['h'], texels, cmode, palette)
        except (TextureError, KeyError, struct.error) as error:
            skipped[identity] = str(error)
    keys = sorted(set(bound.values()))
    matched = [key for key in keys if key in known]
    print('%d bound textures (address and state), %d distinct keys, %d in the index, %d not' % (
        len(bound), len(keys), len(matched), len(keys) - len(matched)))
    for identity, key in sorted(bound.items()):
        record = known.get(key)
        if arguments.verbose or record is None:
            print('%s  %08X %-6s %4dx%-4d  %s' % (key, identity[0], FORMAT_NAMES[identity[1]], identity[2], identity[3],
                                               record['name'] if record else 'NOT IN THE INDEX'))
    for identity, reason in sorted(skipped.items()):
        print('skipped   %08X %-6s %4dx%-4d  %s' % (identity[0], FORMAT_NAMES[identity[1]], identity[2], identity[3], reason))


def main(argv=None):
    parser = argparse.ArgumentParser(description='Extract the game\'s textures to PNG and compute replacement keys.')
    commands = parser.add_subparsers(dest='command', required=True)
    lister = commands.add_parser('list', help='the distinct textures: key, format, size, name')
    lister.add_argument('archive')
    lister.add_argument('words', nargs='*', help='only textures whose key or name contains every word')
    lister.add_argument('--block', help='only this block or file, e.g. mission01_1.map')
    lister.set_defaults(run=command_list)
    extractor = commands.add_parser('extract', help='write every texture as PNG, with index.csv and index.json')
    extractor.add_argument('archive')
    extractor.add_argument('output')
    extractor.add_argument('--mips', action='store_true', help='also write the other mip levels (<name>.mipN.png)')
    extractor.add_argument('--jobs', type=int, default=1, help='worker processes for decoding (at most 3)')
    extractor.set_defaults(run=command_extract)
    one = commands.add_parser('one', help='write one texture as PNG')
    one.add_argument('archive')
    one.add_argument('texture', help='a key, an asset path or a file name without .bmp')
    one.add_argument('image')
    one.add_argument('--block', help='look only in this block or file')
    one.add_argument('--level', type=int, default=0, help='mip level (default 0)')
    one.set_defaults(run=command_one)
    capture = commands.add_parser('capture', help='keys of the textures bound in a GE capture trace')
    capture.add_argument('trace', help='the CHILI_GE_TRACE file; the .gecap files are looked up beside it')
    capture.add_argument('index', nargs='?', help='index.json to name the keys')
    capture.add_argument('--verbose', action='store_true', help='print every bound texture, not only unknown keys')
    capture.set_defaults(run=command_capture)
    arguments = parser.parse_args(argv)
    try:
        arguments.run(arguments)
    except (TextureError, props.BlockError, naz.NazError, OSError, struct.error) as error:
        print('extract_textures.py: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
