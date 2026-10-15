#!/usr/bin/env python3
"""Write every texture of the game as a PNG, for the packaged game.

  dump_textures.py [output folder] [--disc <folder or .iso>] [--ini <ChiliNative.ini>] [--mips] [--jobs N]

The disc is the one the game uses: [Disc] Path of ChiliNative.ini in the folder above this one (start
the game once so that it is set), or --disc. It may be the folder of an unpacked disc or the .iso
file; the archive is read inside the .iso, nothing is unpacked. The output folder is "textures"
beside ChiliNative.ini unless one is given. Everything else is extract_textures.py: the folder layout
and index.csv are described in mods/TEXTURES.md.
"""
import argparse
import builtins
import io
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import extract_textures  # noqa: E402
import naz               # noqa: E402
import props             # noqa: E402

ARCHIVE = 'PSP_GAME/USRDIR/assets00.naz'
SECTOR = 2048


class DiscError(Exception):
    pass


def disc_from_ini(ini):
    """[Disc] Path of the ini file; a path without a drive is taken from the ini's folder."""
    try:
        with open(ini, encoding='utf-8', errors='replace') as stream:
            lines = stream.read().splitlines()
    except OSError:
        raise DiscError('%s was not found: start the game once, or give --disc' % ini)
    section, value = '', ''
    for line in lines:
        line = line.strip()
        if line.startswith('['):
            section = line.lower()
        elif section == '[disc]' and line[:1] not in (';', '#') and '=' in line:
            name, _, text = line.partition('=')
            if name.strip().lower() == 'path':
                value = text.strip().strip('"')
    if not value:
        raise DiscError('%s names no disc ([Disc] Path): start the game once, or give --disc' % ini)
    return value if os.path.isabs(value) else os.path.join(os.path.dirname(os.path.abspath(ini)), value)


def find_in_iso(stream, wanted):
    """Byte offset and size of a file inside a plain ISO 9660 image."""
    def read(offset, count):
        stream.seek(offset)
        data = stream.read(count)
        if len(data) != count:
            raise DiscError('the disc image is cut short')
        return data

    descriptor = read(16 * SECTOR, SECTOR)
    if descriptor[:6] != b'\x01CD001':
        raise DiscError('not a plain ISO 9660 image (a .cso has to be converted to .iso first)')
    record = descriptor[156:190]
    for depth, part in enumerate(wanted.lower().split('/')):
        last = depth == wanted.count('/')
        directory = read((struct.unpack_from('<I', record, 2)[0] + record[1]) * SECTOR, struct.unpack_from('<I', record, 10)[0])
        cursor, found = 0, None
        while cursor < len(directory):
            length = directory[cursor]
            if not length:
                cursor = (cursor // SECTOR + 1) * SECTOR
                continue
            item = directory[cursor:cursor + length]
            cursor += length
            name = item[33:33 + item[32]].decode('latin-1').split(';')[0].rstrip('.').lower()
            if name == part and bool(item[25] & 2) != last:
                found = item
                break
        if found is None:
            raise DiscError('the disc image has no %s' % wanted)
        record = found
    return (struct.unpack_from('<I', record, 2)[0] + record[1]) * SECTOR, struct.unpack_from('<I', record, 10)[0]


class Window(io.RawIOBase):
    """A read-only file that is a byte range of another file."""

    def __init__(self, path, offset, size):
        super().__init__()
        self._stream = builtins.open(path, 'rb', buffering=0)
        self._offset, self._size, self._position = offset, size, 0

    def readable(self):
        return True

    def seekable(self):
        return True

    def tell(self):
        return self._position

    def seek(self, offset, whence=io.SEEK_SET):
        base = 0 if whence == io.SEEK_SET else self._position if whence == io.SEEK_CUR else self._size
        if base + offset < 0:
            raise OSError('seek before the start of the file')
        self._position = base + offset
        return self._position

    def readinto(self, buffer):
        count = max(0, min(len(buffer), self._size - self._position))
        if not count:
            return 0
        self._stream.seek(self._offset + self._position)
        data = self._stream.read(count)
        buffer[:len(data)] = data
        self._position += len(data)
        return len(data)

    def close(self):
        if not self.closed:
            self._stream.close()
        super().close()


def main(argv=None):
    parser = argparse.ArgumentParser(description='Write every texture of the game as a PNG.')
    parser.add_argument('output', nargs='?', help='output folder (default: textures beside ChiliNative.ini)')
    parser.add_argument('--disc', help='disc folder or .iso (default: [Disc] Path of ChiliNative.ini)')
    parser.add_argument('--ini', default=os.path.join(os.path.dirname(HERE), 'ChiliNative.ini'))
    parser.add_argument('--mips', action='store_true', help='also write the smaller mip levels')
    parser.add_argument('--jobs', type=int, default=1, help='worker processes for decoding (at most 3)')
    arguments = parser.parse_args(argv)
    try:
        disc = os.path.abspath(arguments.disc) if arguments.disc else disc_from_ini(arguments.ini)
        output = arguments.output or os.path.join(os.path.dirname(os.path.abspath(arguments.ini)), 'textures')
        if os.path.isdir(disc):
            archive = os.path.join(disc, *ARCHIVE.split('/'))
            if not os.path.isfile(archive):
                raise DiscError('%s is not a disc folder: it has no %s' % (disc, ARCHIVE))
        elif os.path.isfile(disc):
            with builtins.open(disc, 'rb') as stream:
                offset, size = find_in_iso(stream, ARCHIVE)
            # The tools open the archive by name: give them the piece of the image under a name of its own.
            archive = os.path.join(disc, 'assets00.naz')

            def open_archive(path, mode='r', *more, **named):
                if path == archive and 'b' in mode and not set(mode) & set('wax+'):
                    return io.BufferedReader(Window(disc, offset, size))
                return builtins.open(path, mode, *more, **named)

            for module in (extract_textures, naz, props):
                module.open = open_archive
        else:
            raise DiscError('the disc %s was not found' % disc)
    except DiscError as error:
        print('dump_textures.py: %s' % error, file=sys.stderr)
        return 1
    print('disc: %s' % disc)
    print('output: %s' % os.path.abspath(output))
    command = ['extract', archive, output] + (['--mips'] if arguments.mips else [])
    if arguments.jobs > 1:
        command += ['--jobs', str(arguments.jobs)]
    return extract_textures.main(command)


if __name__ == '__main__':
    sys.exit(main())
