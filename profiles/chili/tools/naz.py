#!/usr/bin/env python3
"""List, unpack and pack the game's NAZ archive (assets00.naz).

  naz.py list   <archive> [--tsv]
  naz.py unpack <archive> <output folder> [entry ...]
  naz.py pack   <folder> <archive> [--like <archive>]

NAZ is a ZIP variant (the same container as Total Overdose PC's blocks.naz):

  end record, last 22 bytes   magic 0x16ED5B50, then the ZIP end-of-directory fields
  directory record, 46 bytes  magic 0x0406F370, time, date, crc (0), packed size, size, name length,
                              extra length (1), comment length, disk, attributes, local header offset,
                              then the words 10, 20, 8, 0; the name with every byte rotated right by two
                              bits; one extra byte 0x88
  entry                       30 + name length + 1 bytes of local header the engine never reads, the file's
                              bytes (always stored, never compressed), 12 unused bytes

The engine finds a file from the directory alone: data starts at
local header offset + 30 + name length + extra length. Names are lower case
with forward slashes, e.g. data_psp/textures/menu/loadscreens/generated/bootscreen.texture_psp.

pack writes the files of a folder in name order. With --like the entries the
reference archive also has come first, in its order and with its time stamps,
so an unpacked and repacked archive keeps every file at its original offset.

unpack and pack never write into a game disc folder (one that has
PSP_GAME/SYSDIR/BOOT.BIN) and never overwrite a file that has a second hard
link, so the game's own archive cannot be changed through either of its names.
"""
import argparse
import os
import struct
import sys
import time

END_MAGIC = 0x16ED5B50
RECORD_MAGIC = 0x0406F370
END = struct.Struct('<IHHHHIIH')
RECORD = struct.Struct('<IHHIIIHHHHHIIHHHH')
RECORD_TAIL = (10, 20, 8, 0)
EXTRA = b'\x88'
LOCAL_HEADER = 30
TRAILER = 12
COPY_CHUNK = 1 << 20


class NazError(Exception):
    pass


def guard_output(path):
    """Raises NazError for a path the tools must not write: inside a game disc folder, or over a hard-linked file."""
    path = os.path.abspath(path)
    folder = os.path.dirname(path)
    while True:
        for boot in ('BOOT.BIN', 'EBOOT.BIN'):
            if os.path.isfile(os.path.join(folder, 'PSP_GAME', 'SYSDIR', boot)):
                raise NazError('refusing to write inside the game disc folder %s: %s' % (folder, path))
        parent = os.path.dirname(folder)
        if parent == folder:
            break
        folder = parent
    try:
        links = os.stat(path).st_nlink if os.path.isfile(path) else 1
    except OSError:
        links = 1
    if links > 1:
        raise NazError('refusing to overwrite %s: the file has another hard link, so the same bytes are in use elsewhere' % path)


def _rotate_left(data):
    return bytes(((b << 2) | (b >> 6)) & 0xFF for b in data)


def _rotate_right(data):
    return bytes(((b >> 2) | (b << 6)) & 0xFF for b in data)


class Entry:
    __slots__ = ('name', 'size', 'header_offset', 'time', 'date', 'extra_length')

    def __init__(self, name, size, header_offset, dos_time, dos_date, extra_length=1):
        self.name, self.size, self.header_offset = name, size, header_offset
        self.time, self.date, self.extra_length = dos_time, dos_date, extra_length

    @property
    def data_offset(self):
        return self.header_offset + LOCAL_HEADER + len(self.name.encode('latin-1')) + self.extra_length

    def timestamp(self):
        """The record's DOS time as local seconds since the epoch."""
        year, month, day = 1980 + (self.date >> 9), (self.date >> 5) & 15, self.date & 31
        hour, minute, second = self.time >> 11, (self.time >> 5) & 63, (self.time & 31) * 2
        try:
            return time.mktime((year, max(month, 1), max(day, 1), hour, minute, second, 0, 0, -1))
        except (OverflowError, ValueError):
            return None


def read_directory(stream):
    """Returns the entries of an open archive in directory order."""
    stream.seek(0, os.SEEK_END)
    size = stream.tell()
    if size < END.size:
        raise NazError('too small to be a NAZ archive')
    stream.seek(size - END.size)
    magic, _disk, _first, count, total, directory_size, directory_offset, comment = END.unpack(stream.read(END.size))
    if magic != END_MAGIC:
        raise NazError('no NAZ end record (magic %08X)' % magic)
    if count != total or comment or directory_offset + directory_size + END.size != size:
        raise NazError('the end record does not describe this file')
    stream.seek(directory_offset)
    directory = stream.read(directory_size)
    entries, at = [], 0
    for _ in range(count):
        if at + RECORD.size > len(directory):
            raise NazError('directory record runs past the directory')
        (magic, dos_time, dos_date, _crc, packed, unpacked, name_length, extra_length, comment_length,
         _disk, _internal, _external, header_offset, *_tail) = RECORD.unpack_from(directory, at)
        if magic != RECORD_MAGIC:
            raise NazError('bad directory record magic %08X at entry %d' % (magic, len(entries)))
        if packed != unpacked:
            raise NazError('entry %d is compressed, which the game never uses' % len(entries))
        name = _rotate_left(directory[at + RECORD.size:at + RECORD.size + name_length]).decode('latin-1')
        entry = Entry(name, unpacked, header_offset, dos_time, dos_date, extra_length)
        if entry.data_offset + entry.size > directory_offset:
            raise NazError('entry %s runs into the directory' % name)
        entries.append(entry)
        at += RECORD.size + name_length + extra_length + comment_length
    if at != len(directory):
        raise NazError('%d stray bytes after the last directory record' % (len(directory) - at))
    return entries


def directory_bytes(entries):
    """The directory records for entries, as the engine expects them."""
    out = bytearray()
    for entry in entries:
        name = entry.name.encode('latin-1')
        out += RECORD.pack(RECORD_MAGIC, entry.time, entry.date, 0, entry.size, entry.size, len(name), len(EXTRA),
                           0, 0, 0, 0, entry.header_offset, *RECORD_TAIL)
        out += _rotate_right(name) + EXTRA
    return bytes(out)


def end_record(count, directory_size, directory_offset):
    return END.pack(END_MAGIC, 0, 0, count, count, directory_size, directory_offset, 0)


def normal_name(name):
    """Archive spelling of a path: lower case, forward slashes, no leading slash."""
    return name.replace('\\', '/').strip('/').lower()


def _dos_stamp(seconds):
    t = time.localtime(seconds)
    if t.tm_year < 1980:
        return 0, 0x21
    return (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2), ((t.tm_year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday


def _copy(source, target, count):
    while count:
        block = source.read(min(count, COPY_CHUNK))
        if not block:
            raise NazError('unexpected end of file while copying')
        target.write(block)
        count -= len(block)


def command_list(arguments):
    with open(arguments.archive, 'rb') as stream:
        entries = read_directory(stream)
    if arguments.tsv:
        print('name\tsize\tdata_offset')
    for entry in entries:
        if arguments.tsv:
            print('%s\t%d\t%d' % (entry.name, entry.size, entry.data_offset))
        else:
            print('%10d  %s' % (entry.size, entry.name))
    if not arguments.tsv:
        print('%d files, %d bytes' % (len(entries), sum(entry.size for entry in entries)))


def command_unpack(arguments):
    with open(arguments.archive, 'rb') as stream:
        entries = read_directory(stream)
        if arguments.entries:
            by_name = {normal_name(entry.name): entry for entry in entries}
            wanted = []
            for name in arguments.entries:
                if normal_name(name) not in by_name:
                    raise NazError('no such entry: %s' % name)
                wanted.append(by_name[normal_name(name)])
            entries = wanted
        root = os.path.abspath(arguments.output)
        for entry in entries:
            parts = entry.name.replace('\\', '/').split('/')
            if not all(parts) or any(part in ('.', '..') or ':' in part for part in parts):
                raise NazError('refusing to unpack the unsafe name %r' % entry.name)
            target = os.path.join(root, *parts)
            guard_output(target)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            stream.seek(entry.data_offset)
            with open(target, 'wb') as out:
                _copy(stream, out, entry.size)
            stamp = entry.timestamp()
            if stamp is not None:
                os.utime(target, (stamp, stamp))
    print('unpacked %d files, %d bytes, into %s' % (len(entries), sum(entry.size for entry in entries), root))


def command_pack(arguments):
    root = os.path.abspath(arguments.folder)
    if not os.path.isdir(root):
        raise NazError('not a folder: %s' % root)
    guard_output(arguments.archive)
    output = os.path.abspath(arguments.archive)
    if arguments.like and os.path.exists(output) and os.path.samefile(output, arguments.like):
        raise NazError('the output and the --like archive are the same file')
    files = {}
    for folder, _folders, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            if os.path.abspath(path) == output:
                continue
            archive_name = normal_name(os.path.relpath(path, root))
            try:
                archive_name.encode('latin-1')
            except UnicodeEncodeError:
                raise NazError('name is not Latin-1: %s' % archive_name)
            if archive_name in files:
                raise NazError('two files become the same entry: %s' % archive_name)
            files[archive_name] = path
    if not files:
        raise NazError('nothing to pack in %s' % root)
    order, stamps = [], {}
    if arguments.like:
        with open(arguments.like, 'rb') as stream:
            for entry in read_directory(stream):
                name = normal_name(entry.name)
                if name in files and name not in stamps:
                    order.append(name)
                    stamps[name] = (entry.time, entry.date)
    order += sorted(name for name in files if name not in stamps)
    if len(order) > 0xFFFF:
        raise NazError('more than 65535 files')
    entries = []
    with open(arguments.archive, 'wb') as out:
        for name in order:
            path = files[name]
            size = os.path.getsize(path)
            encoded = name.encode('latin-1')
            header_offset = out.tell()
            if header_offset + LOCAL_HEADER + len(encoded) + len(EXTRA) + size + TRAILER > 0x7FFFFFFF:
                raise NazError('the archive would pass 2 GiB')
            dos_time, dos_date = stamps.get(name) or _dos_stamp(os.path.getmtime(path))
            out.write(bytes(LOCAL_HEADER + len(encoded) + len(EXTRA)))
            with open(path, 'rb') as source:
                _copy(source, out, size)
            out.write(bytes(TRAILER))
            entries.append(Entry(name, size, header_offset, dos_time, dos_date))
        directory_offset = out.tell()
        directory = directory_bytes(entries)
        out.write(directory)
        out.write(end_record(len(entries), len(directory), directory_offset))
        total = out.tell()
    print('packed %d files into %s (%d bytes)' % (len(entries), os.path.abspath(arguments.archive), total))


def main(argv=None):
    parser = argparse.ArgumentParser(description='List, unpack and pack NAZ archives (assets00.naz).')
    commands = parser.add_subparsers(dest='command', required=True)
    lister = commands.add_parser('list', help='print the entries')
    lister.add_argument('archive')
    lister.add_argument('--tsv', action='store_true', help='tab separated: name, size, data offset')
    lister.set_defaults(run=command_list)
    unpacker = commands.add_parser('unpack', help='write one, several or all entries into a folder')
    unpacker.add_argument('archive')
    unpacker.add_argument('output')
    unpacker.add_argument('entries', nargs='*', help='entry names; none means everything')
    unpacker.set_defaults(run=command_unpack)
    packer = commands.add_parser('pack', help='build an archive from a folder')
    packer.add_argument('folder')
    packer.add_argument('archive')
    packer.add_argument('--like', metavar='ARCHIVE', help='keep the entry order and time stamps of this archive')
    packer.set_defaults(run=command_pack)
    arguments = parser.parse_args(argv)
    try:
        arguments.run(arguments)
    except (NazError, OSError) as error:
        print('naz.py: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
