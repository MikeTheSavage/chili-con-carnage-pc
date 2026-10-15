#!/usr/bin/env python3
"""Tests for extract_textures.py: the hash, the decoders, the key and four small textures of the game.

  test_extract_textures.py [assets00.naz]

Without an argument nothing of the game is read: the four textures are stored
below as the bytes the archive holds for them. Their keys are the ones a GE
capture of the running game gave for the same textures in guest memory.
With the real archive as argument the whole archive is also read: counts,
the block walk against props.py, and the four textures found by key.
"""
import base64
import os
import random
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract_textures as et  # noqa: E402
import props                   # noqa: E402
import texture_psp             # noqa: E402

checks = 0

REAL = [
    {'name': '/data/Textures/Hud/HUD_PSP/HealthbarLeftWhite.bmp', 'format': 4, 'swizzled': True,
     'levels': [(0, 32, 8, 8)],
     'key': '4a8db24110b7592b', 'rgba': '3aba747ebbf65c1e',
     'pixels': (
         'AAAAzAAAAAAAAAAAAAAAAAAAAMcAAAAAAAAAAAAAAAAAAABYAAAAAAAAAAAAAAAAAAAAQgAAAAAAAAAAAAAAAAAAAKEAAAAAAAAA'
         'AAAAAAAAAAA7AAAAAAAAAAAAAAAAAAAAaQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA='),
     'palette': (
         '////AObm5v/s7Oz/yMjI//Dw8P/09PT/oqKi//X19f/v7+//vb29/+np6f/V1dX/+fn5/wAAAAAAAAAAAAAAAA==')},
    {'name': '/data/textures/characters/animal_chicken/chicken_dynamite.bmp', 'format': 5, 'swizzled': True,
     'levels': [(0, 16, 16, 16)],
     'key': 'e5e40aeca6c9cca3', 'rgba': '0afa73f8137023c9',
     'pixels': (
         'bW1tbm5uV2tra25ubm1tVG1tbW5uV25ra2tXbm5tbVRtbW1ubm5ua2trbm5ubW1UZGRkampqalxcXGpqamRkKmZmZmlpaWlWVlZp'
         'aWlmZhZiYmJlRGVlWFhYZURlYmIcY2NjaGhoaFNTU2hoaGNjKFstIAUBCCVKWV1sbGxsOx8iIRkmPzg+IyRAYWFhYV9fKxUECgY0'
         'ORAyN1VVVR1fX0hNG1BJDRJCFxFBZ0FnT08YPQMxDzULGixnYGBgYExMSE0eUEkzLkIpL2dnZxRRUScTPDY6AgwwAA5eXl5eQ0Nb'
         'S1JORkVHSlldbGxsbFoJW0tSTkZFRwdZXWxsbGxaWg=='),
     'palette': (
         'Ghoa/+3FAP/w1h7/+N6X/++4Hv/ttAD/78Yd//bhLv/u0wX/ClQA/++6Hv/58KD/8t0s//bnfv8kJCT/+OWY//fkR/+AgID/+OuG'
         '/8aZHP+ampr/xpof/8x2a/96enr/4s+Z/+2uAf/886v/9tV6/9OKf/9WVlb/9tR5/+2wAP/trwD/v40D/7iIA//34TH/AQEB//HZ'
         'Ff/tsgH/v5Qc/9mXjv95eXn/vHJo/8CWH/+Xl5f/wY4C//jrhP9/f3//9+RD//jfl/8dHR3/9ud8//DWIf/47Jn/77ka/ycnJ//u'
         '0gb/890v/+/FGv/tsgD/77Ya/+XRmf/x2Rb/7cEB/wwMDP+cnJz/+u+R/xnlAP+kQTX/7tEF/+2/AP/x2RT/2sF7//bdev/34TD/'
         'v40C/9X/0P/exHv/7bEA//X/9P/21nr/vf+1/+2uAP/16wX/sW9n/1VVVf+lngT/chkP/9TLBf8AAAD/CVUA/7iIAv+NhwP/CwsL'
         '/1NTU///////sbGx/0BAQP+8Szv/xF9R/5gnF/+kQDX/sS0b/5ubm/+uVkz/lSEU/4AcEf99eAP/Pz8//4ciFf9xGQ//AAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==')},
    {'name': '/data/textures/props/cutscene_props/ramp.bmp', 'format': 8, 'swizzled': False,
     'levels': [(0, 16, 16, 16), (128, 8, 8, 8)],
     'key': 'd703a29f46ebcc5e', 'rgba': '568a53e80e647b91',
     'pixels': (
         'F9bW1s2DRmovLS0t6HqEQXR0dncpgyZiU/Ojo8hyxFHU1NSUSoNnai0tLS3IekM5TGxsZGdq5VHZ2dnZy5MGWpaWFhaMi4dyLS0t'
         'L+h6ZEHs5OTkR2KkSZOTkzMpgwZaFhaXl4yDp3I1JScnKXvlUWTEhIRGYsVRO5qa2CmDh3IwEBAQqHKEQWyup8eocgVakJAQMOl6'
         'xUnlhQWF6HoGWgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=='),
     'palette': (
         '')},
    {'name': '/data/Textures/effects/BulletHoles/BulletHole_Small01.BMP', 'format': 9, 'swizzled': False,
     'levels': [(0, 16, 16, 16), (256, 8, 8, 8)],
     'key': '460f85e7785c39db', 'rgba': '11b6e15ad50e3942',
     'pixels': (
         'CKhq9tKUSHMAAAAAAAAAQYCAWFZNjKEoAABRuIO8qYsAAiUlLoxAEAAAixV3R6dnCgsviVSlB2sAABEBEQFUAqpoaHCNlOI4AJNg'
         'pnBpYFlsXlVVoSgAAHynZ2iEZ4X7NVVVVaVJAABnmGW5Z2Z7WSstDQFzrWNBegeaCKsDeQJ6aujq9L3AMEA2UGdgdkCZVVVWVgAQ'
         'AABm/3VmQ5c1ZVVVVS3GcgAAn1lZVmWUNIUCq60qcq2DSXgBaQG8BZYJaurimvKcaHsw1xBEAAAAAFeoAACNlAAIiTaImWNIAAAt'
         'CQoCMKUBQVOZmJVmNgAAoPjXV/S1T4xYAkkFIQMAAABgWFwNjEAQAFEQp3GIc5YCKSUl0ZwgCBUAaBOWSWgZXFjwAG6UIAhStoNz'
         'cXgAMgUtAwCvnAAIaRhlSoYmIxEAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA'
         'AAAAAAAAAAAA'),
     'palette': (
         '')},
]


def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def equal(got, want, what):
    require(got == want, '%s: got %r, want %r' % (what, got, want))


def hash_vectors():
    # Published XXH64 results (the xxHash reference and the python-xxhash documentation).
    for data, seed, want in ((b'', 0, 0xEF46DB3751D8E999), (b'a', 0, 0xD24EC4F1A98C6E5B), (b'abc', 0, 0x44BC2CF5AD770999),
                             (b'xxhash', 0, 0x32DD38952C4BC720), (b'xxhash', 20141025, 0xB559B98D844E0635),
                             (b'Nobody inspects the spammish repetition', 0, 0xFBCEA83C8A378BF1)):
        equal(et.xxh64(data, seed), want, 'xxh64(%r, %d)' % (data, seed))
    # Every tail length after whole stripes takes the same path as a split call would not: only self-consistency here.
    data = bytes(range(256)) * 2
    seen = {et.xxh64(data[:length]) for length in range(0, 80)}
    equal(len(seen), 80, 'xxh64 of 80 prefixes are distinct')


def direct_formats():
    def one(fmt, pixel):
        return tuple(et.decode_level(struct.pack('<H', pixel), fmt, 1, 1))
    equal(one(0, 0xFFFF), (255, 255, 255, 255), '5650 white')
    equal(one(0, 0x001F), (255, 0, 0, 255), '5650 red is the low five bits')
    equal(one(0, 0x07E0), (0, 255, 0, 255), '5650 green')
    equal(one(0, 0xF800), (0, 0, 255, 255), '5650 blue')
    equal(one(0, 0x0010), (132, 0, 0, 255), '5650 replicates the high bits')
    equal(one(1, 0x8000), (0, 0, 0, 255), '5551 alpha bit')
    equal(one(1, 0x7FFF), (255, 255, 255, 0), '5551 without alpha')
    equal(one(1, 0x83E0), (0, 255, 0, 255), '5551 green')
    equal(one(2, 0xF00F), (255, 0, 0, 255), '4444 red and alpha')
    equal(one(2, 0x1234), (0x44, 0x33, 0x22, 0x11), '4444 nibbles')
    equal(et.decode_level(bytes((1, 2, 3, 4, 5, 6, 7, 8)), 3, 2, 1), bytes((1, 2, 3, 4, 5, 6, 7, 8)), '8888 is R G B A')


def paletted_formats():
    palette = b''.join(bytes((i, (100 + i) & 255, (200 - i) & 255, 255 - i)) for i in range(256))
    # CLUT4: the low nibble is the first texel.
    equal(et.decode_level(bytes((0x21, 0xF0)), 4, 4, 1, palette),
          bytes((1, 101, 199, 254, 2, 102, 198, 253, 0, 100, 200, 255, 15, 115, 185, 240)), 'CLUT4 nibble order')
    equal(et.decode_level(bytes((7, 250)), 5, 2, 1, palette), bytes((7, 107, 193, 248, 250, 94, 206, 5)), 'CLUT8')
    # Palette mode: shift 4, mask 15, start 1 (entry 16 on), 5650 entries.
    cmode = 0 | (4 << 2) | (0x0F << 8) | (1 << 16)
    equal(et.palette_entry(0x35, cmode), 19, 'palette entry with shift, mask and start')
    equal(et.palette_entry(0x1FF, 0xFF03), 0xFF, 'a 32-bit palette wraps at 256 entries')
    equal(et.palette_entry(0xFF, 0xFF00 | (0x10 << 16)), 0x1FF, 'a 16-bit palette wraps at 512 entries')
    sixteen = bytearray(64)
    struct.pack_into('<H', sixteen, 19 * 2, 0x07E0)
    equal(et.decode_level(bytes((0x35,)), 5, 1, 1, bytes(sixteen), cmode), bytes((0, 255, 0, 255)), 'CLUT8 through a 5650 palette')
    equal(et.decode_level(struct.pack('<H', 0x0105), 6, 1, 1, palette), bytes((5, 105, 195, 250)), 'CLUT16 uses the masked index')
    equal(et.decode_level(struct.pack('<I', 0x00010207), 7, 1, 1, palette), bytes((7, 107, 193, 248)), 'CLUT32 uses the masked index')
    # An entry past the palette memory reads as nothing.
    equal(et.decode_level(bytes((200,)), 5, 1, 1, palette[:64]), bytes(4), 'entry outside the palette')


def dxt_formats():
    def rows(block, fmt, width=4, height=4):
        rgba = et.decode_level(block, fmt, width, height)
        return [tuple(rgba[i:i + 4]) for i in range(0, len(rgba), 4)]
    # PSP layout: four selector bytes, then the two endpoints.
    four = bytes((0xE4, 0x00, 0x55, 0xFF)) + struct.pack('<HH', 0xF800, 0x001F)
    out = rows(four, 8)
    equal(out[0:4], [(248, 0, 0, 255), (0, 0, 248, 255), (165, 0, 82, 255), (82, 0, 165, 255)], 'DXT1 four colours')
    equal(out[4], (248, 0, 0, 255), 'DXT1 second row')
    equal(out[8], (0, 0, 248, 255), 'DXT1 third row')
    equal(out[12], (82, 0, 165, 255), 'DXT1 fourth row')
    three = bytes((0xE4, 0, 0, 0)) + struct.pack('<HH', 0x001F, 0xF800)
    equal(rows(three, 8)[0:4], [(0, 0, 248, 255), (248, 0, 0, 255), (124, 0, 124, 255), (0, 0, 0, 0)],
          'DXT1 three colours and transparent')
    equal(rows(bytes(4) + struct.pack('<HH', 0x07E0, 0), 8)[0], (0, 252, 0, 255), 'DXT endpoints are not bit-replicated')
    # DXT3: the alpha rows follow the colour block; colours are always four.
    alpha = struct.pack('<HHHH', 0xF840, 0, 0, 0xFFFF)
    out = rows(three + alpha, 9)
    equal(out[0:4], [(0, 0, 248, 0), (248, 0, 0, 68), (82, 0, 165, 136), (165, 0, 82, 255)], 'DXT3 alpha and four colours')
    equal(out[15], (0, 0, 248, 255), 'DXT3 last texel')
    # DXT5: six selector bytes, then the two alpha endpoints.
    selectors = (0 | (1 << 3) | (2 << 6) | (7 << 9)).to_bytes(6, 'little')
    out = rows(four + selectors + bytes((255, 0)), 10)
    equal([c[3] for c in out[0:4]], [255, 0, 218, 36], 'DXT5 eight alphas')
    equal(et._dxt5_alphas(255, 0), [255, 0, 218, 182, 145, 109, 72, 36], 'DXT5 alpha ramp of eight')
    equal(et._dxt5_alphas(0, 255), [0, 255, 51, 102, 153, 204, 0, 255], 'DXT5 alpha ramp of six')
    # Two blocks side by side and a texture narrower than a block.
    equal(rows(four + three, 8, 8, 4)[4], (0, 0, 248, 255), 'DXT1 second block')
    equal(rows(four, 8, 2, 2), [(248, 0, 0, 255), (0, 0, 248, 255), (248, 0, 0, 255), (248, 0, 0, 255)], 'DXT1 2x2')
    # A buffer wider than the texture: the second row of blocks starts a buffer row later.
    data = four + bytes(8) + three + bytes(8)
    equal(et.texel_bytes(data, 0, 8, False, 8, 4, 8), four + three, 'DXT blocks skip the buffer padding')


def storage():
    generator = random.Random(11)
    # 32-bit, 8 pixels wide, 16 rows: one swizzle block is 16 bytes by 8 rows.
    image = generator.randbytes(8 * 16 * 4)
    swizzled = texture_psp.swizzle(image, 32, 16)
    equal(et.texel_bytes(swizzled, 0, 3, True, 8, 8, 16), image, 'unswizzle 8888')
    equal(et.texel_bytes(b'xx' + swizzled, 2, 3, True, 8, 8, 16), image, 'unswizzle at an offset')
    equal(et.texel_bytes(image, 0, 3, False, 8, 8, 16), image, 'linear 8888')
    # CLUT4, 64 wide (32 bytes per row, two blocks).
    image = generator.randbytes(32 * 16)
    equal(et.texel_bytes(texture_psp.swizzle(image, 32, 16), 0, 4, True, 64, 64, 16), image, 'unswizzle CLUT4')
    # CLUT4 8 wide in a 32 wide buffer: 4 of 16 bytes per row are texels.
    texels = generator.randbytes(4 * 8)
    padded = b''.join(texels[y * 4:y * 4 + 4] + generator.randbytes(12) for y in range(8))
    other = b''.join(texels[y * 4:y * 4 + 4] + generator.randbytes(12) for y in range(8))
    equal(et.texel_bytes(padded, 0, 4, True, 32, 8, 8), texels, 'padding is not part of the texels')
    equal(et.texel_bytes(other, 0, 4, True, 32, 8, 8), texels, 'other padding, same texels')
    equal(et.texel_bytes(texels, 0, 4, False, 8, 8, 8), texels, 'linear, no padding')
    # Width 1: only the low nibble is a texel.
    equal(et.texel_bytes(bytes((0xA5, 0x17)), 0, 4, False, 2, 1, 2), bytes((0x05, 0x07)), 'CLUT4 width 1 keeps the low nibble')
    equal(et.texel_indices(bytes((0x05, 0x07)), 4, 1, 2), bytes((5, 7)), 'CLUT4 width 1 indices')
    try:
        et.texel_bytes(bytes(100), 0, 5, False, 16, 16, 16)
        require(False, 'short data was accepted')
    except et.TextureError:
        require(True, '')
    equal(et.storage_bytes(4, True, 32, 8), 128, 'a swizzled level is padded to 16 bytes by 8 rows')
    equal(et.storage_bytes(8, False, 8, 16), 64, 'DXT1 storage follows the buffer width')
    equal((et.key_rows(3, 512, 512), et.key_rows(3, 512, 256), et.key_rows(5, 512, 512)), (272, 256, 512), 'screen image rows')


def key_definition():
    generator = random.Random(3)
    texels = generator.randbytes(32 * 16)                       # CLUT4 64x16
    palette = generator.randbytes(64)
    header = b'CTK1' + bytes((4, 3, 0, 0)) + struct.pack('<HH', 64, 16) + bytes(4)
    equal(len(header), 16, 'key header size')
    want = et.xxh64(palette, et.xxh64(texels, et.xxh64(header)))    # all sixteen entries occur in 512 random bytes
    equal(set(et.texel_indices(texels, 4, 64, 16)), set(range(16)), 'the sample uses every entry')
    equal(et.texture_key(4, 64, 16, texels, 0xFF03, palette), want, 'key of a CLUT4 texture')
    # Palette memory behind the sixteen entries is never read.
    equal(et.texture_key(4, 64, 16, texels, 0xFF03, palette + generator.randbytes(960)), want, 'memory behind the palette')
    # Only entries that occur count.
    few = bytes((0x10, 0x32)) * 8                                # indices 0..3, 32x1
    first = et.texture_key(4, 32, 1, few, 0xFF03, palette)
    changed = bytearray(palette)
    changed[4 * 4] ^= 0xFF                                       # entry 4 does not occur
    equal(et.texture_key(4, 32, 1, few, 0xFF03, bytes(changed)), first, 'an entry that does not occur')
    changed[2 * 4] ^= 0xFF                                       # entry 2 does
    require(et.texture_key(4, 32, 1, few, 0xFF03, bytes(changed)) != first, 'a used palette entry changes the key')
    equal(et.used_palette(et.texel_indices(few, 4, 32, 1), palette, 0xFF03), palette[:16], 'used entries in index order')
    require(et.texture_key(4, 32, 1, few, 0xFF03, palette) != et.texture_key(4, 16, 2, few, 0xFF03, palette), 'the size is in the key')
    direct = generator.randbytes(16 * 4 * 4)
    plain = b'CTK1' + bytes((3, 0, 0, 0)) + struct.pack('<HH', 16, 4) + bytes(4)
    equal(et.texture_key(3, 16, 4, direct), et.xxh64(b'', et.xxh64(direct, et.xxh64(plain))), 'key without a palette')
    require(et.texture_key(3, 16, 4, direct) != et.texture_key(7, 16, 4, direct, 0xFF03, bytes(1024)), 'the format is in the key')
    # The same texels stored swizzled, padded or linear give one key.
    texels = generator.randbytes(4 * 8)
    padded = b''.join(texels[y * 4:y * 4 + 4] + generator.randbytes(12) for y in range(8))
    a = et.texture_key(4, 8, 8, et.texel_bytes(padded, 0, 4, True, 32, 8, 8), 0xFF03, palette)
    b = et.texture_key(4, 8, 8, et.texel_bytes(texels, 0, 4, False, 8, 8, 8), 0xFF03, palette)
    equal(a, b, 'storage layout is not in the key')
    try:
        et.texture_key(5, 1, 1, bytes((40,)), 0xFF03, palette)
        require(False, 'a missing palette entry was accepted')
    except et.TextureError:
        require(True, '')


def real_texture(fixture):
    texture = et.Texture('block', 'test', fixture['name'])
    texture.format, texture.swizzled = fixture['format'], fixture['swizzled']
    texture.levels = [et.Level(*level) for level in fixture['levels']]
    texture.pixels = base64.b64decode(''.join(fixture['pixels']))
    texture.palette = base64.b64decode(''.join(fixture['palette']))
    return texture


def real_textures():
    for fixture in REAL:
        texture = real_texture(fixture)
        equal('%016x' % texture.key(), fixture['key'], 'key of %s' % fixture['name'])
        width, rows, rgba = texture.rgba()
        equal((width, rows), tuple(fixture['levels'][0][2:4]), 'size of %s' % fixture['name'])
        equal('%016x' % et.xxh64(rgba), fixture['rgba'], 'pixels of %s' % fixture['name'])
        for number in range(1, len(texture.levels)):
            width, rows, rgba = texture.rgba(number)
            equal(len(rgba), width * rows * 4, 'mip level %d of %s' % (number, fixture['name']))
    health = real_texture(REAL[0])                 # CLUT4 8x8 in a 32 wide swizzled buffer
    _width, _rows, rgba = health.rgba()
    equal(tuple(rgba[0:4]), (255, 255, 255, 0), 'health bar, first texel is transparent')
    equal(tuple(rgba[6 * 4:7 * 4]), (249, 249, 249, 255), 'health bar, texel 6 of the first row')
    equal(tuple(rgba[7 * 4:8 * 4]), (249, 249, 249, 255), 'health bar, texel 7 of the first row')


def standalone_file(root):
    # A .texture_psp file made here: 16x8, 32-bit, swizzled, pixels inside the record.
    generator = random.Random(8)
    image = generator.randbytes(16 * 8 * 4)
    name = b'/data/textures/test/sample.bmp\0\0'
    commands = struct.pack('<9I', 0xC2000001, 0xC3000003, 0xC6000101, 0xC8E80000, 0xA0000000, 0xA8000010,
                           0xB8000304, 0xCB000000, 0x0B000000)
    gfx_at = 0x30 + len(name)
    pixels_at = gfx_at + 16 + len(commands) + 12
    record = bytearray(pixels_at)
    struct.pack_into('<II', record, 0, 0, 0x2C)
    struct.pack_into('<I', record, 0x1C, 0x18)
    struct.pack_into('<I', record, 0x28, gfx_at - 0x28)
    record[0x30:0x30 + len(name)] = name
    struct.pack_into('<iiiH', record, gfx_at, pixels_at - gfx_at, 0, 8, len(image) // 128)
    record[gfx_at + 16:gfx_at + 16 + len(commands)] = commands
    swizzled = texture_psp.swizzle(image, 64, 8)
    data = struct.pack('<III', 1, len(record) + len(swizzled), 0) + bytes(11) + bytes(record) + swizzled
    texture = et.standalone_texture(data, 'sample.texture_psp')
    equal((texture.name, texture.format, texture.swizzled, len(texture.levels)), ('/data/textures/test/sample.bmp', 3, True, 1),
          'standalone file state')
    equal((texture.width, texture.height, texture.place), (16, 8, 'record'), 'standalone file size')
    equal(texture.rgba()[2], image, 'standalone file pixels')
    equal(et.image_path(texture.name), 'data/textures/test/sample', 'image path')
    # The same file through texture_psp.py gives the same pixels.
    equal(texture_psp.decode(data), (16, 8, image), 'texture_psp.py agrees')
    target = os.path.join(root, 'sample.png')
    texture_psp.write_png(target, 16, 8, texture.rgba()[2])
    equal(texture_psp.read_png(target), (16, 8, image), 'PNG round trip')
    # A chain whose next level does not halve ends there, as in the renderer.
    broken = bytearray(data)
    at = 23 + gfx_at + 16
    struct.pack_into('<I', broken, at, 0xC2010001)                 # two levels declared, no level 1 commands
    equal(len(et.standalone_texture(bytes(broken), 'x').levels), 1, 'a level without commands ends the chain')


def real_archive(path):
    entries, occurrences, problems, census = et.build_index(path, level_keys=True)
    equal(problems, [], 'unreadable textures')
    equal(occurrences, 7333, 'textures stored in the archive')
    equal(len(entries), 1363, 'distinct keys')
    equal((census['archive entries'], census['blocks'], census['asset records'], census['texture records'],
           census['font records'], census['standalone files'], census['stray GE list heads']),
          (395, 172, 25032, 7274, 2, 57, 0), 'census')
    formats = {}
    for entry in entries.values():
        name = et.FORMAT_NAMES[entry.texture.format]
        formats[name] = formats.get(name, 0) + 1
    equal(formats, {'8888': 57, 'CLUT4': 791, 'CLUT8': 87, 'DXT1': 219, 'DXT3': 209}, 'distinct textures by format')
    # One asset path never has two contents.
    paths = {}
    for entry in entries.values():
        for name in entry.names:
            paths.setdefault(name.lower(), set()).add(entry.key)
    equal(max(len(keys) for keys in paths.values()), 1, 'keys per asset path')
    equal(len(paths), 1399, 'asset paths')
    for fixture in REAL:
        require(fixture['key'] in entries, '%s is not in the archive under its key' % fixture['name'])
        stored = entries[fixture['key']].texture
        equal(stored.pixels, real_texture(fixture).pixels, 'stored bytes of %s' % fixture['name'])
        equal('%016x' % et.xxh64(stored.rgba()[2]), fixture['rgba'], 'archive pixels of %s' % fixture['name'])
    # The block walk finds the records props.py finds, in four blocks of different kinds.
    archive = props.Archive(path)
    for name in ('todpsp_game.main', 'todpsp_game_uk.main', 'mainmenuscene.map', 'mission01_1_uk.map'):
        entry = archive.find(name)
        data = archive.read(entry)
        is_map = entry.name.lower().endswith('.map')
        block, assets = et.Block(data, is_map), props.read_block(data, is_map)
        equal([(record.type, record.name, record.size) for record in block.records],
              [(asset.type, asset.name, asset.size if asset.offset >= block.info + block.info_size else 0)
               for asset in assets], 'records of %s' % name)


def main():
    hash_vectors()
    direct_formats()
    paletted_formats()
    dxt_formats()
    storage()
    key_definition()
    real_textures()
    with tempfile.TemporaryDirectory() as root:
        standalone_file(root)
    if len(sys.argv) > 1:
        real_archive(sys.argv[1])
    print('test_extract_textures: %d checks passed%s' % (checks, '' if len(sys.argv) > 1 else ' (no archive given)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
