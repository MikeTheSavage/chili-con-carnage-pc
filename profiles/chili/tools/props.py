#!/usr/bin/env python3
"""List the gameplay values a mod's properties.ini can change.

  props.py blocks    <assets00.naz>
  props.py fragments <assets00.naz> [--block todpsp_game.main]
  props.py list      <assets00.naz> [--block todpsp_game.main] [fragment ...] [--all]

The engine keeps its tunable values as properties of entities. The values are
not in database.bin (that file only declares the 5,710 property names and
their types); they are "set property" records of the fragment assets inside
the .main and .map blocks. The format is the one of the same engine's blocks in
Total Overdose for the PC, with a different block header on PSP:

  block     base 0 (.main) or 299 (.map: GUID 36, fingerprint 255, two words)
            +0   u32 time stamp, property checksum, command checksum, i32 region, u32 asset count,
                 u32 info size, u32 largest asset
            +28  u8 flag, u32 stream start, u32 stream end, then (end - start) stream bytes
            then run-length words (low 31 bits a number of assets) that add up to the asset count
            then the info buffer (the asset records), asset count sizes, the payloads in record order
  asset     +0 type (4 is a fragment), +4 self-relative pointer to the name, +28 the value 24 or 25;
            a fragment whose size in the table is 0 keeps its records inside its own asset record,
            at the self-relative pointer +36
  fragment  24 byte header whose first word is the offset of the first chunk; a chunk is
            u32 offset to the next chunk (0 or 0x80000000 ends), u32 byte length of its records,
            u32 self-relative pointer to the first record
  record    u32 entity id, u32 packed; packed >> 24 is 0xFF for "create" (then a word count and the
            type name), 0xFE for "delete", otherwise the low 24 bits are a property index and the
            value follows: one word for number, integer, truth and Entity, three for vector, four
            for quaternion, a word count and padded text for string
  index     0..709 are properties the engine registers itself (16 is the entity's name);
            710 + n is entry n of database.bin

list prints what properties.ini takes: a [fragment] line, then Entity.Property = value lines
(#<number>.Property where the entity has no name of its own). Only number, integer, truth,
vector, quaternion and string values can be changed; --all also shows the others as comments.
Nothing is written anywhere: copy the lines you want into mods/<name>/properties.ini.
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import naz  # noqa: E402

BUILTIN_PROPERTIES = 710
NAME_PROPERTY = 16
FRAGMENT_TYPE = 4
MAP_BASE = 299
DEFAULT_BLOCK = 'todpsp_game.main'
DATABASE = 'data_baked/scripts/stable/database.bin'
FIXED_WORDS = {'number': 1, 'integer': 1, 'truth': 1, 'Entity': 1, 'vector': 3, 'quaternion': 4}
EDITABLE = ('number', 'integer', 'truth', 'vector', 'quaternion', 'string')


class BlockError(Exception):
    pass


def read_database(data):
    """The (name, type) pairs of database.bin's property section, in file order."""
    if len(data) < 4:
        raise BlockError('database.bin is too small')
    count, at, out = struct.unpack_from('<I', data, 0)[0], 4, []
    for _ in range(count):
        if at + 4 > len(data):
            raise BlockError('database.bin ends inside its property list')
        length = struct.unpack_from('<I', data, at)[0]
        at += 4
        if length < 1 or at + length > len(data):
            raise BlockError('database.bin has a bad entry length')
        name, _colon, kind = data[at:at + length - 1].decode('latin-1').rpartition(':')
        out.append((name, kind))
        at += length
    return out


class Asset:
    __slots__ = ('type', 'name', 'offset', 'size')

    def __init__(self, kind, name, offset, size):
        self.type, self.name, self.offset, self.size = kind, name, offset, size


def read_block(data, is_map):
    """The assets of a block with the place of each payload. Raises BlockError unless the whole file is accounted for."""
    base = MAP_BASE if is_map else 0
    if len(data) < base + 37:
        raise BlockError('too small to be a block')
    count, info_size = struct.unpack_from('<II', data, base + 16)
    stream_start, stream_end = struct.unpack_from('<II', data, base + 29)
    if stream_end < stream_start:
        raise BlockError('bad stream range')
    at, total = base + 37 + (stream_end - stream_start), 0
    while total < count:
        if at + 4 > len(data):
            raise BlockError('the run-length words run past the file')
        total += struct.unpack_from('<I', data, at)[0] & 0x7FFFFFFF
        at += 4
    info, sizes_at = at, at + info_size
    payload = sizes_at + 4 * count
    if total != count or payload > len(data):
        raise BlockError('the header does not describe this file')
    sizes = struct.unpack_from('<%dI' % count, data, sizes_at)
    if payload + sum(sizes) != len(data):
        raise BlockError('the asset sizes do not add up to the file')
    assets = []
    for at in range(info, sizes_at - 31, 4):
        kind, pointer, zero8, _, zero16, zero20, flags, marker = struct.unpack_from('<8I', data, at)
        if kind > 12 or zero8 or zero16 or zero20 or flags & 0xFFFF or marker | 1 != 0x19:
            continue
        name_at = at + 4 + (pointer & ~3)
        if name_at >= sizes_at or data[name_at:name_at + 1] != b'/':
            continue
        end = data.find(b'\0', name_at, min(sizes_at, name_at + 512))
        if end < 0 or end - name_at < 5 or not all(32 <= c < 127 for c in data[name_at:end]):
            continue
        assets.append((at, kind, data[name_at:end].decode('ascii')))
    if len(assets) != count:
        raise BlockError('found %d asset records, the header says %d' % (len(assets), count))
    out = []
    for number, ((record, kind, name), size) in enumerate(zip(assets, sizes)):
        asset = Asset(kind, name, payload, size)
        limit = assets[number + 1][0] if number + 1 < count else sizes_at
        if kind == FRAGMENT_TYPE and not size and limit - record >= 64:
            # No payload: the fragment sits inside its own record, up to the next record.
            pointer = struct.unpack_from('<I', data, record + 36)[0]
            if 0 < pointer < 0x80000000 and record + 36 + pointer + 24 <= limit:
                asset.offset, asset.size = record + 36 + pointer, limit - (record + 36 + pointer)
        out.append(asset)
        payload += size
    return out


class Value:
    """One set-property record: where its value is and what it is."""
    __slots__ = ('entity', 'index', 'offset', 'words')

    def __init__(self, entity, index, offset, words):
        self.entity, self.index, self.offset, self.words = entity, index, offset, words


class Fragment:
    def __init__(self):
        self.entities = []      # entity ids in creation order
        self.types = {}         # entity id -> type text
        self.names = {}         # entity id -> name
        self.values = []        # Value, in file order


def read_fragment(data, start, size, database):
    """Walks a fragment's records. Raises BlockError when a record does not start where one is expected."""
    end = start + size
    limit = BUILTIN_PROPERTIES + len(database)

    def word(at):
        return struct.unpack_from('<I', data, at)[0]

    if size < 24 or 0 < word(start) < 24:
        raise BlockError('too small to be a fragment')
    chunks, chunk = [], start + (word(start) or size)
    while chunk + 12 <= end:
        following, length = word(chunk), word(chunk + 4)
        first = chunk + 8 + word(chunk + 8)
        if first < chunk + 12 or first + length > end:
            raise BlockError('a chunk runs outside the fragment')
        chunks.append((first, first + length))
        if following in (0, 0x80000000) or following > end - chunk or len(chunks) > 4096:
            break
        chunk += following
    fragment = Fragment()
    for first, stop in chunks:
        at = first
        while at + 12 <= stop and word(at + 4) >> 24 in (0xFE, 0xFF):
            if word(at + 4) >> 24 == 0xFE:
                at += 8
                continue
            entity, words = word(at), word(at + 8)
            if at + 12 + 4 * words > stop:
                raise BlockError('a create record runs past its chunk')
            if entity not in fragment.types:
                fragment.entities.append(entity)
            fragment.types[entity] = data[at + 12:at + 12 + 4 * words].split(b'\0', 1)[0].decode('latin-1')
            at += 12 + 4 * words

    def starts_record(at, stop):
        if at + 8 > stop or word(at) not in fragment.types:
            return False
        packed = word(at + 4)
        return packed >> 24 in (0xFE, 0xFF) or packed < limit

    for first, stop in chunks:
        at = first
        while at + 8 <= stop:
            if not starts_record(at, stop):
                raise BlockError('no record at offset %d' % (at - start))
            entity, packed = word(at), word(at + 4)
            at += 8
            if packed >> 24 == 0xFE:
                continue
            if packed >> 24 == 0xFF:
                at += 4 + 4 * word(at)
                continue
            kind = database[packed - BUILTIN_PROPERTIES][1] if packed >= BUILTIN_PROPERTIES else None
            words = FIXED_WORDS.get(kind)
            if kind == 'string':
                words = 1 + word(at) if at + 4 <= stop else None
            if words is None:
                # A property the engine registers itself, a list or a struct: the value runs to the next record.
                words = 0
                while at + 4 * words + 4 <= stop and not starts_record(at + 4 * words, stop):
                    words += 1
            if at + 4 * words > stop:
                raise BlockError('a value runs past its chunk')
            if packed == NAME_PROPERTY and words >= 2 and word(at) == words - 1:
                fragment.names[entity] = data[at + 4:at + 4 * words].split(b'\0', 1)[0].decode('latin-1')
            fragment.values.append(Value(entity, packed, at, words))
            at += 4 * words
    return fragment


def format_value(data, value, kind):
    def numbers(count):
        return ', '.join('%.9g' % x for x in struct.unpack_from('<%df' % count, data, value.offset))
    if kind == 'number':
        return numbers(1)
    if kind == 'integer':
        return '%d' % struct.unpack_from('<i', data, value.offset)[0]
    if kind == 'truth':
        return 'true' if struct.unpack_from('<I', data, value.offset)[0] & 1 else 'false'
    if kind == 'vector':
        return numbers(3)
    if kind == 'quaternion':
        return numbers(4)
    if kind == 'string':
        return '"%s"' % data[value.offset + 4:value.offset + 4 * value.words].split(b'\0', 1)[0].decode('latin-1')
    return ' '.join('%08X' % w for w in struct.unpack_from('<%dI' % min(value.words, 8), data, value.offset)) + \
        (' ...' if value.words > 8 else '')


def short_name(path):
    """/data/Scripts/fragments/DamageModDict.fragment -> DamageModDict"""
    name = path.rsplit('/', 1)[-1]
    return name[:-len('.fragment')] if name.lower().endswith('.fragment') else name


class Archive:
    def __init__(self, path):
        self.stream = open(path, 'rb')
        self.entries = naz.read_directory(self.stream)

    def read(self, entry):
        self.stream.seek(entry.data_offset)
        return self.stream.read(entry.size)

    def find(self, name):
        wanted = naz.normal_name(name)
        matches = [entry for entry in self.entries
                   if naz.normal_name(entry.name) == wanted or naz.normal_name(entry.name).endswith('/' + wanted)]
        if not matches:
            raise BlockError('the archive has no %s' % name)
        shared = [entry for entry in matches if '/shared/' in naz.normal_name(entry.name)]
        return (shared or matches)[0]

    def blocks(self):
        return [entry for entry in self.entries if entry.name.lower().endswith(('.main', '.map'))]


def command_blocks(arguments):
    archive = Archive(arguments.archive)
    for entry in archive.blocks():
        try:
            assets = read_block(archive.read(entry), entry.name.lower().endswith('.map'))
            fragments = sum(1 for asset in assets if asset.type == FRAGMENT_TYPE and asset.size)
            print('%4d assets  %4d fragments  %s' % (len(assets), fragments, entry.name))
        except BlockError as error:
            print('   not readable (%s)  %s' % (error, entry.name))


def command_fragments(arguments):
    archive = Archive(arguments.archive)
    entry = archive.find(arguments.block)
    data = archive.read(entry)
    database = read_database(archive.read(archive.find(DATABASE)))
    for asset in read_block(data, entry.name.lower().endswith('.map')):
        if asset.type != FRAGMENT_TYPE:
            continue
        if not asset.size:
            print('%8s  %-40s  %s (no records)' % ('-', short_name(asset.name), asset.name))
            continue
        try:
            fragment = read_fragment(data, asset.offset, asset.size, database)
            print('%8d  %-40s  %s (%d entities, %d values)' % (
                asset.size, short_name(asset.name), asset.name, len(fragment.entities), len(fragment.values)))
        except BlockError as error:
            print('%8d  %-40s  %s (not readable: %s)' % (asset.size, short_name(asset.name), asset.name, error))


def command_list(arguments):
    archive = Archive(arguments.archive)
    entry = archive.find(arguments.block)
    data = archive.read(entry)
    database = read_database(archive.read(archive.find(DATABASE)))
    wanted = [name.lower() for name in arguments.fragments]
    shown = 0
    for asset in read_block(data, entry.name.lower().endswith('.map')):
        if asset.type != FRAGMENT_TYPE or not asset.size:
            continue
        if wanted and not any(asset.name.lower().endswith(('/' + name, '/' + name + '.fragment')) for name in wanted):
            continue
        try:
            fragment = read_fragment(data, asset.offset, asset.size, database)
        except BlockError as error:
            print('; %s: not readable (%s)\n' % (asset.name, error))
            continue
        shown += 1
        block = '' if naz.normal_name(entry.name).endswith('/' + DEFAULT_BLOCK) else entry.name.rsplit('/', 1)[-1] + ' : '
        print('[%s%s]    ; %s, %d entities' % (block, short_name(asset.name), asset.name, len(fragment.entities)))
        order = {entity: number for number, entity in enumerate(fragment.entities)}
        # A name several entities share would change all of them, so those are listed by number.
        named = {}
        for entity, name in fragment.names.items():
            named[name.lower()] = named.get(name.lower(), 0) + 1
        last = None
        for value in fragment.values:
            if value.index < BUILTIN_PROPERTIES:
                continue
            name, kind = database[value.index - BUILTIN_PROPERTIES]
            if kind not in EDITABLE and not arguments.all:
                continue
            if value.entity != last:
                last = value.entity
                print('; %s #%d %s' % (fragment.types.get(value.entity, '?'), order.get(value.entity, -1),
                                       fragment.names.get(value.entity, '')))
            entity = fragment.names.get(value.entity)
            if not entity or named[entity.lower()] > 1 or entity != entity.strip() or entity[0] in '[#' or '=' in entity or ';' in entity:
                entity = '#%d' % order.get(value.entity, -1)
            line = '%s.%s = %s' % (entity, name, format_value(data, value, kind))
            print(line if kind in EDITABLE else '; %s    (%s, cannot be changed)' % (line, kind))
        print()
    if not shown:
        raise BlockError('no such fragment in %s; "props.py fragments" lists them' % entry.name)


def main(argv=None):
    parser = argparse.ArgumentParser(description='List the gameplay values a properties.ini can change.')
    commands = parser.add_subparsers(dest='command', required=True)
    blocks = commands.add_parser('blocks', help='the .main and .map blocks of the archive')
    blocks.add_argument('archive')
    blocks.set_defaults(run=command_blocks)
    fragments = commands.add_parser('fragments', help='the fragments of one block')
    fragments.add_argument('archive')
    fragments.add_argument('--block', default=DEFAULT_BLOCK)
    fragments.set_defaults(run=command_fragments)
    lister = commands.add_parser('list', help='the values of one block, or of the named fragments')
    lister.add_argument('archive')
    lister.add_argument('fragments', nargs='*')
    lister.add_argument('--block', default=DEFAULT_BLOCK)
    lister.add_argument('--all', action='store_true', help='also show values that cannot be changed, as comments')
    lister.set_defaults(run=command_list)
    arguments = parser.parse_args(argv)
    try:
        arguments.run(arguments)
    except (BlockError, naz.NazError, OSError, struct.error) as error:
        print('props.py: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
