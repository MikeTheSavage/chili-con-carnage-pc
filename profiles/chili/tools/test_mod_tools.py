#!/usr/bin/env python3
"""Tests for naz.py, texture_psp.py, make_sample_mod.py and props.py on files made here.

  test_mod_tools.py [assets00.naz]

With the real archive as argument it also lists it, repacks a few of its files,
checks the boot screen texture converts to an image and back unchanged, and
reads every block and fragment for the values properties.ini can change.
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_sample_mod  # noqa: E402
import naz              # noqa: E402
import props            # noqa: E402
import texture_psp      # noqa: E402

checks = 0


def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise AssertionError(message)


def tool(*arguments, expect=0):
    result = subprocess.run([sys.executable, os.path.join(HERE, arguments[0])] + list(arguments[1:]), capture_output=True, text=True)
    require(result.returncode == expect, '%s exited %d: %s' % (' '.join(arguments), result.returncode, result.stderr.strip()))
    return result.stdout


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as out:
        out.write(data)


def read(path):
    with open(path, 'rb') as stream:
        return stream.read()


def archive_round_trip(root):
    generator = random.Random(5)
    files = {
        'facecoll.mat': generator.randbytes(224),
        'data_psp/textures/menu/boot.texture_psp': generator.randbytes(70000),
        'data_psp/maps/m1/m1.map': generator.randbytes(200001),
        'data/engine/save_assets/icon0.png': generator.randbytes(1),
    }
    source = os.path.join(root, 'source')
    for name, data in files.items():
        write(os.path.join(source, *name.split('/')), data)
    os.utime(os.path.join(source, 'facecoll.mat'), (1168600000, 1168600000))
    first = os.path.join(root, 'first.naz')
    require('packed 4 files' in tool('naz.py', 'pack', source, first), 'pack output')
    with open(first, 'rb') as stream:
        entries = naz.read_directory(stream)
        require([entry.name for entry in entries] == sorted(files), 'pack order is name order')
        for entry in entries:
            stream.seek(entry.data_offset)
            require(stream.read(entry.size) == files[entry.name], 'packed data of ' + entry.name)
        stream.seek(-22, os.SEEK_END)
        require(struct.unpack('<I', stream.read(4))[0] == 0x16ED5B50, 'end record magic')
    listing = tool('naz.py', 'list', first)
    require('4 files, %d bytes' % sum(len(data) for data in files.values()) in listing and 'data_psp/maps/m1/m1.map' in listing, 'list output')
    rows = tool('naz.py', 'list', first, '--tsv').splitlines()
    require(rows[0] == 'name\tsize\tdata_offset' and len(rows) == 5, 'tsv listing')

    everything = os.path.join(root, 'everything')
    tool('naz.py', 'unpack', first, everything)
    for name, data in files.items():
        require(read(os.path.join(everything, *name.split('/'))) == data, 'unpacked ' + name)
    require(abs(os.path.getmtime(os.path.join(everything, 'facecoll.mat')) - 1168600000) <= 2, 'unpack restores the time stamp')
    one = os.path.join(root, 'one')
    tool('naz.py', 'unpack', first, one, 'DATA_PSP\\Maps\\M1\\m1.map')
    require(read(os.path.join(one, 'data_psp', 'maps', 'm1', 'm1.map')) == files['data_psp/maps/m1/m1.map'] and
            not os.path.exists(os.path.join(one, 'facecoll.mat')), 'unpack of one entry')
    tool('naz.py', 'unpack', first, one, 'no/such/file', expect=1)

    # A repack of the unpacked folder is the same archive, byte for byte.
    second = os.path.join(root, 'second.naz')
    tool('naz.py', 'pack', everything, second)
    require(read(second) == read(first), 'unpack then pack changed the archive')

    # --like keeps the reference order and time stamps; new files go last.
    shuffled = os.path.join(root, 'shuffled.naz')
    order = ['data_psp/maps/m1/m1.map', 'facecoll.mat', 'data/engine/save_assets/icon0.png', 'data_psp/textures/menu/boot.texture_psp']
    with open(shuffled, 'wb') as out:
        made = []
        for index, name in enumerate(order):
            header = out.tell()
            out.write(b'\xee' * (30 + len(name) + 1) + files[name] + b'\xdd' * 12)
            made.append(naz.Entry(name, len(files[name]), header, 0x6000 + index, 0x362C))
        directory = naz.directory_bytes(made)
        start = out.tell()
        out.write(directory + naz.end_record(len(made), len(directory), start))
    write(os.path.join(everything, 'data_psp', 'zz_new.bin'), b'new')
    write(os.path.join(everything, 'data_psp', 'maps', 'm1', 'm1.map'), b'changed size')
    third = os.path.join(root, 'third.naz')
    tool('naz.py', 'pack', everything, third, '--like', shuffled)
    with open(third, 'rb') as stream:
        entries = naz.read_directory(stream)
    require([entry.name for entry in entries] == order + ['data_psp/zz_new.bin'], '--like order')
    require([entry.time for entry in entries[:4]] == [0x6000, 0x6001, 0x6002, 0x6003] and entries[0].size == 12, '--like time stamps')

    # Broken input is refused.
    data = bytearray(read(first))
    data[-6] ^= 0x40
    write(os.path.join(root, 'broken.naz'), bytes(data))
    tool('naz.py', 'list', os.path.join(root, 'broken.naz'), expect=1)
    tool('naz.py', 'pack', os.path.join(root, 'nothing-here'), os.path.join(root, 'x.naz'), expect=1)


def make_texture(width, height, rgba, swizzled=True):
    header = b'\xfa\x4e\xa2\x0d' + bytes(0x30) + b'/data/textures/menu/test.bmp\x00' + bytes(9)
    commands = struct.pack('<9I', 0xC2000000 | int(swizzled), 0xC3000003, 0xC6000101, 0xC8E80000, 0xA0000000,
                           0xA8000000 | width, 0xB8000909, 0xCB000000, 0x0B000000)
    pixels = texture_psp.swizzle(rgba, width * 4, height) if swizzled else rgba
    return header + commands + bytes(12) + pixels


def texture_round_trip(root):
    generator = random.Random(9)
    width, height = 64, 24
    rgba = generator.randbytes(width * height * 4)
    require(texture_psp.unswizzle(texture_psp.swizzle(rgba, width * 4, height), width * 4, height) == rgba, 'swizzle round trip')
    # One 16-byte, 8-row block of the swizzled data is one 4x8 pixel tile.
    tile = texture_psp.swizzle(rgba, width * 4, height)[:128]
    require(tile == b''.join(rgba[row * width * 4:row * width * 4 + 16] for row in range(8)), 'swizzle block layout')
    for swizzled in (True, False):
        original = make_texture(width, height, rgba, swizzled)
        path = os.path.join(root, 'test_%d.texture_psp' % swizzled)
        write(path, original)
        require(texture_psp.decode(original) == (width, height, rgba), 'decode')
        require('%dx%d' % (width, height) in tool('texture_psp.py', 'info', path), 'info output')
        png = os.path.join(root, 'test_%d.png' % swizzled)
        tool('texture_psp.py', 'to-png', path, png)
        require(texture_psp.read_png(png) == (width, height, rgba), 'PNG round trip')
        again = os.path.join(root, 'again_%d.texture_psp' % swizzled)
        tool('texture_psp.py', 'from-png', png, path, again)
        require(read(again) == original, 'texture -> PNG -> texture changed the file')
    # A recoloured image goes in; the header and the GE commands stay.
    recoloured = make_sample_mod.recolour(512, 272, bytes([10, 20, 30, 255]) * (512 * 272))
    require(recoloured[:4] == bytes([30, 20, 10, 255]) and recoloured != bytes([30, 20, 10, 255]) * (512 * 272), 'recolour swaps red and blue and draws a label')
    wrong = os.path.join(root, 'wrong.png')
    texture_psp.write_png(wrong, 32, 24, bytes(32 * 24 * 4))
    tool('texture_psp.py', 'from-png', wrong, os.path.join(root, 'test_1.texture_psp'), os.path.join(root, 'no.texture_psp'), expect=1)
    write(os.path.join(root, 'not_a_texture.bin'), bytes(4096))
    tool('texture_psp.py', 'info', os.path.join(root, 'not_a_texture.bin'), expect=1)


def write_guards(root):
    """The tools refuse to write into a disc folder or over a file that has a second hard link."""
    disc = os.path.join(root, 'disc0')
    usrdir = os.path.join(disc, 'PSP_GAME', 'USRDIR')
    source = os.path.join(root, 'source')
    write(os.path.join(source, 'facecoll.mat'), bytes(range(200)))
    write(os.path.join(source, 'data_psp', 'a.bin'), bytes(1000))
    work = os.path.join(root, 'work.naz')
    tool('naz.py', 'pack', source, work)
    write(os.path.join(disc, 'PSP_GAME', 'SYSDIR', 'BOOT.BIN'), b'~PSP')
    os.makedirs(usrdir)
    archive = os.path.join(usrdir, 'assets00.naz')
    os.replace(work, archive)
    original = read(archive)
    linked = os.path.join(root, 'disc0_fl1', 'PSP_GAME', 'USRDIR', 'assets00.naz')
    os.makedirs(os.path.dirname(linked))
    os.link(archive, linked)                      # a second disc folder as the frame_lock 1 runs use it
    loose = os.path.join(root, 'copy.naz')
    os.link(archive, loose)
    for target in (archive, linked, loose, os.path.join(usrdir, 'new.naz'), os.path.join(disc, 'x', 'y.naz')):
        tool('naz.py', 'pack', source, target, expect=1)
        tool('naz.py', 'pack', source, target, '--like', archive, expect=1)
    tool('naz.py', 'unpack', archive, usrdir, expect=1)
    tool('naz.py', 'unpack', archive, os.path.join(disc, 'out'), expect=1)
    os.makedirs(os.path.join(root, 'out'))
    os.link(archive, os.path.join(root, 'out', 'facecoll.mat'))   # unpacking over it would write through the link
    tool('naz.py', 'unpack', archive, os.path.join(root, 'out'), expect=1)
    for target in (archive, loose, os.path.join(usrdir, 'p.png')):
        try:
            texture_psp.write_png(target, 4, 4, bytes(64))
            require(False, 'write_png wrote ' + target)
        except naz.NazError:
            require(True, '')
    require(read(archive) == original and read(linked) == original and os.stat(archive).st_nlink == 4, 'the archive changed')
    require(sorted(os.listdir(usrdir)) == ['assets00.naz'] and not os.path.exists(os.path.join(disc, 'out')) and
            not os.path.exists(os.path.join(disc, 'x')), 'something was written into the disc folder')
    # A mod's own disc0 overlay is not a game disc folder: packing an archive there is the documented use.
    mod_archive = os.path.join(root, 'mods', 'm', 'disc0', 'PSP_GAME', 'USRDIR', 'mod.naz')
    os.makedirs(os.path.dirname(mod_archive))
    tool('naz.py', 'pack', source, mod_archive)
    tool('naz.py', 'pack', source, mod_archive, '--like', archive)
    tool('naz.py', 'pack', source, mod_archive, '--like', mod_archive, expect=1)


def block_fixture():
    """A .main block with one fragment: two entities, a name each, three database values."""
    def words(*values):
        return struct.pack('<%dI' % len(values), *values)

    def text(value, count):
        return words(count) + value.encode('ascii').ljust(4 * count, b'\0')
    database = [('Speed', 'number'), ('Count', 'integer'), ('Label', 'string')]
    base = props.BUILTIN_PROPERTIES
    first, second = 0x10000000, 0x10000001
    creates = words(first, 0xFF000000) + text('thing(Node)', 3) + words(second, 0xFF000000) + text('thing(Node)', 3)
    creates += words(first, 13) + struct.pack('<3f', 1, 2, 3) + words(first, props.NAME_PROPERTY) + text('First', 2)
    values = words(first, base) + struct.pack('<f', 1.5) + words(first, base + 1, 7) + words(first, base + 2) + text('hello', 2)
    values += words(second, props.NAME_PROPERTY) + text('First', 2) + words(second, base) + struct.pack('<f', 2.5)
    fragment = words(24, 0, 0x80000000, 0x2800, 0, 0)
    fragment += words(12 + len(creates), len(creates), 4) + creates + words(0x80000000, len(values), 4) + values
    assets = [(0, '/data/textures/a.bmp', bytes(100)), (4, '/data/Scripts/fragments/Tuning.fragment', fragment),
              (4, '/data/Scripts/fragments/Empty.fragment', words(0, 0, 0x80000000, 0x2800, 0, 0))]
    info = b''
    for kind, name, _payload in assets:
        record = words(kind, 44, 0, 0x6D696E61, 0, 0, 0x69730000, 24, 0, 0xFFFFFFDC, 0, 0) + name.encode('ascii')
        info += record.ljust(128, b'\0')
    # A fragment without a payload: its records sit inside its asset record, behind the pointer at +36.
    head = words(4, 44, 0, 0x6D696E61, 0, 0, 0x69730000, 24, 0, 64, 0, 0) + b'/data/CustomizeFragments/Weapons/Inside.fragment'
    record = head.ljust(100, b'\0') + fragment
    info += record.ljust(-(-len(record) // 16) * 16, b'\0')
    sizes = [len(payload) for _kind, _name, payload in assets] + [0]
    block = words(1, 2, 3, 0, len(sizes), len(info), 0) + b'\x01' + words(0, 0) + words(len(sizes)) + info
    block += words(*sizes) + b''.join(payload for _kind, _name, payload in assets)
    return database, block


def property_listing():
    database, block = block_fixture()
    raw = struct.pack('<I', len(database)) + b''.join(
        struct.pack('<I', len(name) + len(kind) + 2) + ('%s:%s' % (name, kind)).encode('ascii') + b'\0' for name, kind in database)
    require(props.read_database(raw + struct.pack('<I', 0)) == database, 'database.bin properties')
    assets = props.read_block(block, False)
    require([(asset.type, asset.name, asset.size) for asset in assets][1][:2] == (4, '/data/Scripts/fragments/Tuning.fragment') and
            len(assets) == 4, 'block assets')
    inside = props.read_fragment(block, assets[3].offset, assets[3].size, database)
    require(assets[3].size > 0 and inside.entities == [0x10000000, 0x10000001] and len(inside.values) == 7, 'a fragment inside its asset record')
    fragment = props.read_fragment(block, assets[1].offset, assets[1].size, database)
    require(fragment.entities == [0x10000000, 0x10000001] and fragment.names == {0x10000000: 'First', 0x10000001: 'First'} and
            len(fragment.values) == 7, 'fragment walk')
    shown = [(props.format_value(block, value, database[value.index - props.BUILTIN_PROPERTIES][1]))
             for value in fragment.values if value.index >= props.BUILTIN_PROPERTIES]
    require(shown == ['1.5', '7', '"hello"', '2.5'], 'fragment values: %r' % shown)
    require(len(props.read_fragment(block, assets[2].offset, assets[2].size, database).values) == 0, 'empty fragment')
    for damaged in (block[:20], block[:16] + struct.pack('<I', 5) + block[20:], block + b'\0'):
        try:
            props.read_block(damaged, False)
            require(False, 'a damaged block was read')
        except (props.BlockError, struct.error):
            require(True, '')


def real_properties(archive):
    opened = props.Archive(archive)
    database = props.read_database(opened.read(opened.find(props.DATABASE)))
    require(len(database) == 5710 and database[0] == ('default_Gravity', 'vector'), 'database.bin of the real archive')
    blocks = fragments = values = 0
    for entry in opened.blocks():
        data = opened.read(entry)
        blocks += 1
        for asset in props.read_block(data, entry.name.lower().endswith('.map')):
            if asset.type == props.FRAGMENT_TYPE and asset.size:
                values += len(props.read_fragment(data, asset.offset, asset.size, database).values)
                fragments += 1
    require(blocks == 172 and fragments > 3000 and values > 3000000, 'blocks %d fragments %d values %d' % (blocks, fragments, values))
    listing = tool('props.py', 'list', archive, 'DamageModDict')
    require('[DamageModDict]' in listing and 'ShellDamageMod.StructureDamMod = 5' in listing, 'props.py list DamageModDict')
    listing = tool('props.py', 'list', archive, 'Weapon_Gun_Light', '--block', 'mission01_1.map')
    require('[mission01_1.map : Weapon_Gun_Light]' in listing and 'Gun_Light.Clipsize_m = 10' in listing, 'props.py list of a level block')
    require('DamageModDict' in tool('props.py', 'fragments', archive) and 'todpsp_game.main' in tool('props.py', 'blocks', archive), 'props.py fragments and blocks')
    tool('props.py', 'list', archive, 'NoSuchFragment', expect=1)
    return blocks, fragments, values


def real_archive(archive, root):
    with open(archive, 'rb') as stream:
        entries = naz.read_directory(stream)
        directory = naz.directory_bytes(entries)
        stream.seek(0, os.SEEK_END)
        start = stream.tell() - 22 - len(directory)
        stream.seek(start)
        require(stream.read() == directory + naz.end_record(len(entries), len(directory), start), 'regenerated directory differs from the real one')
    require(len(entries) == 395, 'the real archive lists %d files, not 395' % len(entries))
    require(('%d files' % len(entries)) in tool('naz.py', 'list', archive), 'list of the real archive')
    boot = make_sample_mod.BOOT_SCREEN
    picked = [boot, 'facecoll.mat', 'data_baked/scripts/stable/database.bin']
    out = os.path.join(root, 'real')
    tool('naz.py', 'unpack', archive, out, *picked)
    small = os.path.join(root, 'small.naz')
    tool('naz.py', 'pack', out, small, '--like', archive)
    with open(small, 'rb') as packed, open(archive, 'rb') as stream:
        repacked = naz.read_directory(packed)
        by_name = {entry.name: entry for entry in entries}
        require([entry.name for entry in repacked] == [entry.name for entry in entries if entry.name in picked], 'repack order follows the real archive')
        for entry in repacked:
            was = by_name[entry.name]
            packed.seek(entry.data_offset)
            stream.seek(was.data_offset)
            require((entry.size, entry.time, entry.date) == (was.size, was.time, was.date) and packed.read(entry.size) == stream.read(was.size),
                    'repacked ' + entry.name)
    texture = read(os.path.join(out, *boot.split('/')))
    width, height, rgba = texture_psp.decode(texture)
    require((width, height) == (512, 272) and texture_psp.encode(texture, width, height, rgba) == texture, 'boot screen texture round trip')
    mods = os.path.join(root, 'mods')
    tool('make_sample_mod.py', archive, mods)
    made = read(os.path.join(mods, 'sample_bootscreen', *boot.split('/')))
    require(len(made) == len(texture) and made[:0xB7] == texture[:0xB7] and made != texture, 'sample mod texture')


def main():
    with tempfile.TemporaryDirectory(prefix='chili_mod_tools_') as root:
        archive_round_trip(os.path.join(root, 'naz'))
        texture_round_trip(os.path.join(root, 'texture'))
        write_guards(os.path.join(root, 'guards'))
        property_listing()
        real = ''
        if len(sys.argv) > 1:
            real_archive(sys.argv[1], os.path.join(root, 'real'))
            real = ', real archive included: %d blocks, %d fragments, %d property values read' % real_properties(sys.argv[1])
    print('PASS: naz.py list/unpack/pack, texture_psp.py conversions, sample mod image, write guards, props.py (%d checks)%s' % (checks, real))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except AssertionError as error:
        print('FAIL: %s' % error, file=sys.stderr)
        sys.exit(1)
