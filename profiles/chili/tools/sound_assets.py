#!/usr/bin/env python3
"""Find, name and extract the game's streamed sounds (ATRAC3plus in RIFF/WAVE).

  sound_assets.py list    <assets00.naz> [word ...] [--tsv]
  sound_assets.py extract <assets00.naz> <name or key> <output.at3> [--block NAME] [--raw]
  sound_assets.py trace   <assets00.naz> <run.log>

The engine has two kinds of sound. Short effects and the enemies' shouts are ADPCM
samples played by the firmware mixer (sceSasCore voices). Everything else is a
"media stream": an ATRAC3plus RIFF file that the game feeds to the firmware decoder
(sceAtrac) through a small ring buffer and mixes itself. Streams are in three places
of assets00.naz:

  * standalone files, data_stream_psp/.../<name>.mediastream_data_psp (music, ambience,
    a few cutscene tracks),
  * stream assets (type 8, names ending .stream) of a .map block, whose bytes are in the
    block's second segment (props.py calls it the stream): the record holds the pair
    <segment address> <bytes>, the address counted from the block header's stream start,
  * stream assets of a .main block, whose bytes follow the PSP file name inside the record.

Stereo streams are music, ambience and the sound of the in-engine cutscenes ("zapps").
One-channel streams are speech: the player's lines (uk_g11_speak_ram_001, the taunts)
are in the language blocks (uk/fr/es), 280 bytes per frame; one-channel streams of 192
bytes per frame in the shared blocks are announcer and effect lines.

A standalone file can hold several streams of the same length side by side (the layers of a combo
music track: the game switches from one to the next at the same sample as the combo grows). Such a
file starts with one block per layer, each a RIFF header and a whole number of frames; after them
come chunks of <u32 chunk bytes> <u32 bytes of layer 0> .. <u32 bytes of layer n-1> followed by that
many bytes of each layer in turn. A long file with one layer is stored the same way. list names
the layers "<name> (layer k)"; extract writes the one asked for as a plain stream (--raw writes the
file as stored).

A stream's key is the CRC-32 of the first 256 bytes of its data chunk. A run with
PSPRECOMP_LOG=sound prints the same key for every stream the game starts ("[sound]
stream ... key=xxxxxxxx"); trace adds the asset names to those lines.

Nothing here needs an extra package and nothing is written except the file extract names.
"""
import argparse
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import naz    # noqa: E402
import props  # noqa: E402

STREAM_TYPE = 8
KEY_BYTES = 256
PSP_SUFFIX = b'.mediastream_data_psp'


class Stream:
    __slots__ = ('block', 'name', 'offset', 'size', 'channels', 'frame_bytes', 'samples', 'data_offset', 'data_size', 'key', 'layer')

    def seconds(self):
        return self.samples / 44100.0


def parse_riff(data, at, limit):
    """A Stream for the RIFF/WAVE at data[at:], or None when it is not an ATRAC3plus stream."""
    if at + 12 > limit or data[at:at + 4] != b'RIFF' or data[at + 8:at + 12] != b'WAVE':
        return None
    size = struct.unpack_from('<I', data, at + 4)[0] + 8
    if at + size > limit:
        return None
    stream = Stream()
    stream.layer = None
    stream.offset, stream.size, stream.samples, stream.channels, stream.frame_bytes = at, size, 0, 0, 0
    stream.data_offset = stream.data_size = 0
    p = at + 12
    while p + 8 <= min(at + size, len(data)):
        tag, length = data[p:p + 4], struct.unpack_from('<I', data, p + 4)[0]
        if tag == b'fmt ' and length >= 16:
            code, stream.channels, _rate, _bytes, stream.frame_bytes = struct.unpack_from('<HHIIH', data, p + 8)
            if code != 0xFFFE:
                return None
        elif tag == b'fact' and length >= 4:
            stream.samples = struct.unpack_from('<I', data, p + 8)[0]
        elif tag == b'data':
            stream.data_offset, stream.data_size = p + 8 - at, length
            break
        p += 8 + ((length + 1) & ~1)
    if not stream.channels or not stream.data_offset:
        return None
    first = at + stream.data_offset
    stream.key = zlib.crc32(data[first:first + min(KEY_BYTES, stream.data_size)]) & 0xFFFFFFFF
    return stream


def layer_heads(data):
    """Offsets of the layers' first blocks in a standalone file: [0] for a plain stream."""
    if data[:4] != b'RIFF':
        return [0]
    heads, at = [0], data.find(data[:12], 12)
    block = at
    while at > 0 and at == block * len(heads) and data[at:at + 12] == data[:12]:
        heads.append(at)
        at += block
    return heads


def split_layers(data):
    """The layers of a standalone file, each as a plain RIFF stream."""
    heads = layer_heads(data)
    riff = struct.unpack_from('<I', data, 4)[0] + 8
    count = len(heads)
    if count > 1:
        block = heads[1]
    else:
        # One layer: its first block ends where the first chunk header stands, after whole frames.
        first = parse_riff(data, 0, len(data) + riff)
        block = 0
        if first is not None and len(data) > riff and first.frame_bytes:
            at = first.data_offset + first.frame_bytes
            while at + 8 <= min(len(data), 1 << 20):
                total, size = struct.unpack_from('<II', data, at)
                if total == size + 8 and size and size % first.frame_bytes == 0:
                    block = at
                    break
                at += first.frame_bytes
        if not block:
            return [data]
    parts = [[data[at:at + block]] for at in heads]
    at = block * count
    while at + 4 + 4 * count <= len(data):
        total = struct.unpack_from('<I', data, at)[0]
        sizes = struct.unpack_from('<%dI' % count, data, at + 4)
        if total != 4 + 4 * count + sum(sizes) or at + total > len(data):
            break
        cursor = at + 4 + 4 * count
        for index in range(count):
            parts[index].append(data[cursor:cursor + sizes[index]])
            cursor += sizes[index]
        at += total
    return [b''.join(part)[:riff] for part in parts]


def record_starts(data, is_map):
    """(record offset, type, name) of every asset record of a block, with the segment's place: props.read_block's scan."""
    base = props.MAP_BASE if is_map else 0
    count, info_size = struct.unpack_from('<II', data, base + 16)
    start, end = struct.unpack_from('<II', data, base + 29)
    segment = base + 37
    at, total = segment + (end - start), 0
    while total < count:
        total += struct.unpack_from('<I', data, at)[0] & 0x7FFFFFFF
        at += 4
    info, sizes_at = at, at + info_size
    records = []
    for at in range(info, sizes_at - 31, 4):
        kind, pointer, zero8, _, zero16, zero20, flags, marker = struct.unpack_from('<8I', data, at)
        if kind > 12 or zero8 or zero16 or zero20 or flags & 0xFFFF or marker | 1 != 0x19:
            continue
        name_at = at + 4 + (pointer & ~3)
        if name_at >= sizes_at or data[name_at:name_at + 1] != b'/':
            continue
        stop = data.find(b'\0', name_at, min(sizes_at, name_at + 512))
        if stop < 0 or stop - name_at < 5 or not all(32 <= c < 127 for c in data[name_at:stop]):
            continue
        records.append((at, kind, data[name_at:stop].decode('ascii')))
    return records, sizes_at, segment, start, end


def block_streams(block, data):
    """Every ATRAC3plus stream inside a .map or .main block, named where a stream asset points at it."""
    is_map = block.endswith('.map')
    names = {}
    try:
        records, sizes_at, segment, start, end = record_starts(data, is_map)
    except (struct.error, IndexError):
        records, sizes_at, segment, start, end = [], 0, 0, 0, 0
    for number, (at, kind, name) in enumerate(records):
        if kind != STREAM_TYPE:
            continue
        limit = records[number + 1][0] if number + 1 < len(records) else sizes_at
        for p in range(at + 32, limit - 7, 4):
            address, size = struct.unpack_from('<II', data, p)
            if start <= address < end and 0 < size <= end - address and data[segment + address - start:segment + address - start + 4] == b'RIFF':
                names[segment + address - start] = name
                break
    out, at = [], 0
    while True:
        at = data.find(b'RIFF', at)
        if at < 0:
            break
        stream = parse_riff(data, at, len(data))
        if stream is None:
            at += 4
            continue
        stream.block = block
        stream.name = names.get(at)
        if stream.name is None:
            # A .main block: the PSP file name ends just before two size words and the RIFF.
            window = data[max(0, at - 320):at]
            end_name = window.rfind(PSP_SUFFIX)
            if end_name >= 0:
                begin = window.rfind(b'\0', 0, end_name) + 1
                text = window[begin:end_name + len(PSP_SUFFIX)]
                slash = text.find(b'/')
                stream.name = text[slash:].decode('latin-1') if slash >= 0 else None
        if stream.name is None:
            stream.name = '%s@%d' % (os.path.basename(block), at)
        out.append(stream)
        at += stream.size
    return out


def all_streams(archive):
    """Every stream of the archive, in archive order."""
    out = []
    with open(archive, 'rb') as handle:
        for entry in naz.read_directory(handle):
            name = entry.name
            if name.endswith(PSP_SUFFIX.decode()) or name.endswith('.at3'):
                handle.seek(entry.data_offset)
                head = handle.read(min(entry.size, 1 << 20))
                heads = layer_heads(head)
                for index, at in enumerate(heads):
                    stream = parse_riff(head, at, at + entry.size)
                    if stream is None:
                        break
                    stream.block, stream.name, stream.offset = '', '/' + name + (' (layer %d)' % index if index else ''), 0
                    stream.size = entry.size
                    # A file larger than its stream is stored in chunks, also with one layer.
                    stream.layer = index if len(heads) > 1 or entry.size > stream.data_offset + stream.data_size else None
                    out.append((entry, stream))
            elif name.endswith('.map') or name.endswith('.main'):
                handle.seek(entry.data_offset)
                data = handle.read(entry.size)
                for stream in block_streams(name, data):
                    out.append((entry, stream))
    return out


def read_stream(archive, entry, stream):
    with open(archive, 'rb') as handle:
        handle.seek(entry.data_offset + stream.offset)
        return handle.read(stream.size)


def describe(stream):
    return '%s  %d ch  %d bytes/frame  %.2f s  %d samples  key=%08x' % (
        stream.name, stream.channels, stream.frame_bytes, stream.seconds(), stream.samples, stream.key)


def command_list(arguments):
    words = [word.lower() for word in arguments.words]
    if arguments.tsv:
        print('block\tname\toffset\tbytes\tchannels\tframe_bytes\tsamples\tseconds\tkey')
    for entry, stream in all_streams(arguments.archive):
        text = (stream.block + ' ' + stream.name).lower()
        if any(word not in text for word in words):
            continue
        if arguments.tsv:
            print('%s\t%s\t%d\t%d\t%d\t%d\t%d\t%.3f\t%08x' % (stream.block or entry.name, stream.name, stream.offset, stream.size,
                                                             stream.channels, stream.frame_bytes, stream.samples, stream.seconds(), stream.key))
        else:
            print('%-28s %s' % (os.path.basename(stream.block) or '(file)', describe(stream)))
    return 0


def find(archive, what, block):
    what = what.lower()
    key = int(what, 16) if len(what) == 8 and all(c in '0123456789abcdef' for c in what) else None
    found = []
    for entry, stream in all_streams(archive):
        if block and block.lower() not in (stream.block or entry.name).lower():
            continue
        if (key is not None and stream.key == key) or (key is None and what in stream.name.lower()):
            found.append((entry, stream))
    return found


def command_extract(arguments):
    found = find(arguments.archive, arguments.what, arguments.block)
    if not found:
        print('no stream matches %s' % arguments.what, file=sys.stderr)
        return 1
    # The same file is often stored in several blocks; any copy will do when the bytes agree.
    entry, stream = found[0]
    data = read_stream(arguments.archive, entry, stream)
    if stream.layer is not None and not arguments.raw:
        data = split_layers(data)[stream.layer]
    different = [s for e, s in found[1:] if (s.key, s.size) != (stream.key, stream.size)]
    if different:
        print('%d different streams match; the first is taken. Use --block or the key:' % (len(different) + 1), file=sys.stderr)
        for _, other in [(entry, stream)] + [(None, s) for s in different][:12]:
            print('  %s  %s' % (os.path.basename(other.block) or '(file)', describe(other)), file=sys.stderr)
    naz.guard_output(arguments.output)
    with open(arguments.output, 'wb') as handle:
        handle.write(data)
    print('%s: %s' % (arguments.output, describe(stream)))
    return 0


def command_trace(arguments):
    by_key = {}
    for entry, stream in all_streams(arguments.archive):
        by_key.setdefault(stream.key, stream)
    with open(arguments.log, 'r', errors='replace') as handle:
        for line in handle:
            if not line.startswith('[sound] stream'):
                continue
            line = line.rstrip()
            at = line.find(' key=')
            if at >= 0:
                stream = by_key.get(int(line[at + 5:at + 13], 16))
                line += '  ' + (stream.name if stream else '(not in the archive)')
            print(line)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    lister = commands.add_parser('list')
    lister.add_argument('archive')
    lister.add_argument('words', nargs='*')
    lister.add_argument('--tsv', action='store_true')
    lister.set_defaults(run=command_list)
    extract = commands.add_parser('extract')
    extract.add_argument('archive')
    extract.add_argument('what')
    extract.add_argument('output')
    extract.add_argument('--block', default='')
    extract.add_argument('--raw', action='store_true')
    extract.set_defaults(run=command_extract)
    trace = commands.add_parser('trace')
    trace.add_argument('archive')
    trace.add_argument('log')
    trace.set_defaults(run=command_trace)
    arguments = parser.parse_args()
    try:
        return arguments.run(arguments)
    except (naz.NazError, OSError) as error:
        print('sound_assets.py: %s' % error, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
