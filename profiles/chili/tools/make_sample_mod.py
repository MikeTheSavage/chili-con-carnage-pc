#!/usr/bin/env python3
"""Make the sample mod: a boot screen with swapped colours and a MOD label.

  make_sample_mod.py <assets00.naz> <mods folder> [--name sample_bootscreen]

Reads the boot screen texture out of your own archive, recolours it and writes

  <mods folder>/<name>/data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp

which the game then loads instead of the one in the archive. Nothing but that
one file changes; delete the folder to get the original screen back.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import naz            # noqa: E402
import texture_psp    # noqa: E402

BOOT_SCREEN = 'data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp'
GLYPHS = {
    'M': ('#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'),
    'O': ('.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'),
    'D': ('####.', '#...#', '#...#', '#...#', '#...#', '#...#', '####.'),
}


def recolour(width, height, rgba):
    """Swaps red and blue everywhere and stamps a MOD label in the bottom left corner."""
    pixels = bytearray(rgba)
    pixels[0::4], pixels[2::4] = rgba[2::4], rgba[0::4]
    scale, left, top = 6, 16, height - 7 * 6 - 24
    box = (left - 8, top - 8, left + 3 * 6 * scale + 2, top + 7 * scale + 8)
    for y in range(box[1], box[3]):
        for x in range(box[0], box[2]):
            pixels[(y * width + x) * 4:(y * width + x) * 4 + 4] = b'\x10\x10\x10\xff'
    for index, letter in enumerate('MOD'):
        for row, line in enumerate(GLYPHS[letter]):
            for column, mark in enumerate(line):
                if mark != '#':
                    continue
                for y in range(top + row * scale, top + (row + 1) * scale):
                    for x in range(left + (index * 6 + column) * scale, left + (index * 6 + column + 1) * scale):
                        pixels[(y * width + x) * 4:(y * width + x) * 4 + 4] = b'\xff\xe0\x20\xff'
    return bytes(pixels)


def main(argv=None):
    parser = argparse.ArgumentParser(description='Make the sample boot screen mod from your own assets00.naz.')
    parser.add_argument('archive')
    parser.add_argument('mods')
    parser.add_argument('--name', default='sample_bootscreen')
    arguments = parser.parse_args(argv)
    try:
        with open(arguments.archive, 'rb') as stream:
            entries = {naz.normal_name(entry.name): entry for entry in naz.read_directory(stream)}
            if BOOT_SCREEN not in entries:
                raise naz.NazError('the archive has no %s' % BOOT_SCREEN)
            stream.seek(entries[BOOT_SCREEN].data_offset)
            original = stream.read(entries[BOOT_SCREEN].size)
        width, height, rgba = texture_psp.decode(original)
        target = os.path.join(os.path.abspath(arguments.mods), arguments.name, *BOOT_SCREEN.split('/'))
        naz.guard_output(target)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, 'wb') as out:
            out.write(texture_psp.encode(original, width, height, recolour(width, height, rgba)))
    except (naz.NazError, texture_psp.TextureError, OSError) as error:
        print('make_sample_mod.py: %s' % error, file=sys.stderr)
        return 1
    print('wrote %s' % target)
    return 0


if __name__ == '__main__':
    sys.exit(main())
