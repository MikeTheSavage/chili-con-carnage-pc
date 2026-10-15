#!/usr/bin/env python3
"""Make a texture pack from pictures that keep the game's own file names.

The game replaces a texture when a mod has  textures/<key>.png  (see mods/TEXTURES.md);
the key is a hash, which is no name to work with. This tool lets you work by name:

  init  <extracted folder> <work folder>
        Copies the game's textures (the level-0 pictures written by extract_textures.py,
        without the mip levels) into a work folder under their own names, and remembers
        what each one looked like.

  build <work folder> <mods folder>/<pack name>
        Looks at every PNG in the work folder. A picture that differs from the game's
        original becomes  <pack>/textures/<key>.png ; untouched ones are left out, so
        the pack holds only what you changed. Prints what it did and what it could not
        place.

  check <work folder>
        The same report without writing a pack.

Edit or upscale the pictures in the work folder in place: keep the folder structure and
the file names, keep the shape (width to height), any size up to 8192x8192. A file named
<key>.png anywhere in the work folder is accepted as it is.
"""
import csv
import hashlib
import json
import os
import shutil
import struct
import sys

MANIFEST = 'texture_pack_work.json'
MAX_SIDE = 8192


def fail(message):
    print('error: ' + message, file=sys.stderr)
    sys.exit(1)


def png_size(path):
    with open(path, 'rb') as handle:
        head = handle.read(24)
    if len(head) < 24 or head[:8] != b'\x89PNG\r\n\x1a\n' or head[12:16] != b'IHDR':
        return None
    return struct.unpack('>II', head[16:24])


def file_hash(path):
    digest = hashlib.sha1()
    with open(path, 'rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def read_index(folder):
    index = os.path.join(folder, 'index.csv')
    if not os.path.isfile(index):
        fail('no index.csv in %s (run extract_textures.py extract first)' % folder)
    with open(index, newline='', encoding='utf-8') as handle:
        return [row for row in csv.DictReader(handle) if row.get('png')]


def init(extracted, work):
    rows = read_index(extracted)
    if os.path.exists(os.path.join(work, MANIFEST)):
        fail('%s already holds a work folder; choose another or delete it' % work)
    textures = {}
    copied = 0
    for row in rows:
        relative = row['png'].replace('\\', '/')
        source = os.path.join(extracted, relative)
        if not os.path.isfile(source):
            print('missing in the extraction, skipped: ' + relative)
            continue
        target = os.path.join(work, relative)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(source, target)
        textures[relative.lower()] = {
            'key': row['key'], 'name': row['name'], 'width': int(row['width']), 'height': int(row['height']),
            'original': file_hash(source), 'other_names': row.get('other_names', ''),
        }
        copied += 1
    with open(os.path.join(work, MANIFEST), 'w', encoding='utf-8') as handle:
        json.dump({'version': 1, 'textures': textures}, handle, indent=1)
    print('%d textures copied to %s' % (copied, work))
    print('Edit them in place (same names, same shape, any size), then run: build "%s" <mods folder>/<pack name>' % work)


def scan(work):
    manifest_path = os.path.join(work, MANIFEST)
    if not os.path.isfile(manifest_path):
        fail('%s is not a work folder made by "init" (no %s)' % (work, MANIFEST))
    with open(manifest_path, encoding='utf-8') as handle:
        textures = json.load(handle)['textures']
    keys = {entry['key'] for entry in textures.values()}
    changed, unchanged, problems, by_key = [], 0, [], {}
    for folder, _, files in os.walk(work):
        for name in sorted(files):
            if not name.lower().endswith('.png'):
                continue
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, work).replace('\\', '/')
            stem = name[:-4].lower()
            size = png_size(path)
            if size is None:
                problems.append('%s: not a PNG file' % relative)
                continue
            entry = textures.get(relative.lower())
            if entry is None and stem in keys:
                entry = next(e for e in textures.values() if e['key'] == stem)
                original = None
            elif entry is None:
                if '.mip' not in name.lower():
                    problems.append('%s: no game texture has this name' % relative)
                continue
            else:
                original = entry['original']
            if original is not None and file_hash(path) == original:
                unchanged += 1
                continue
            if max(size) > MAX_SIDE:
                problems.append('%s: %dx%d is larger than %d' % (relative, size[0], size[1], MAX_SIDE))
                continue
            if size[0] * entry['height'] != size[1] * entry['width']:
                problems.append('%s: %dx%d does not keep the shape of the original %dx%d (it will be stretched)'
                                % (relative, size[0], size[1], entry['width'], entry['height']))
            if entry['key'] in by_key:
                problems.append('%s and %s are the same texture in the game; the second one is used'
                                % (by_key[entry['key']][1], relative))
            by_key[entry['key']] = (path, relative, size, entry)
            changed.append(relative)
    return by_key, unchanged, problems


def report(by_key, unchanged, problems):
    pixels = sum(size[0] * size[1] for _, _, size, _ in by_key.values())
    print('%d changed pictures, %d left as the game has them' % (len(by_key), unchanged))
    if by_key:
        print('about %.0f MB of graphics memory when all of them are on screen (with mip levels)' % (pixels * 4 * 4 / 3 / 1e6))
    for line in problems:
        print('note: ' + line)


def build(work, pack):
    by_key, unchanged, problems = scan(work)
    target = os.path.join(pack, 'textures')
    os.makedirs(target, exist_ok=True)
    stale = [n for n in os.listdir(target) if n.lower().endswith('.png') and n[:-4].lower() not in by_key]
    for name in stale:
        os.remove(os.path.join(target, name))
    listing = []
    for key in sorted(by_key):
        path, relative, size, entry = by_key[key]
        shutil.copyfile(path, os.path.join(target, key + '.png'))
        listing.append('%s  %5dx%-5d  %s' % (key, size[0], size[1], relative))
    with open(os.path.join(pack, 'pack.txt'), 'w', encoding='utf-8') as handle:
        handle.write('Texture pack built by build_texture_pack.py: key, size, the name it was made from\n')
        handle.write('\n'.join(listing) + '\n')
    report(by_key, unchanged, problems)
    if stale:
        print('%d pictures of an earlier build removed from the pack' % len(stale))
    print('pack written to %s (%d pictures); the game uses it when its folder is in the mods folder' % (pack, len(by_key)))


def main(arguments):
    if len(arguments) == 3 and arguments[0] == 'init':
        init(arguments[1], arguments[2])
    elif len(arguments) == 3 and arguments[0] == 'build':
        build(arguments[1], arguments[2])
    elif len(arguments) == 2 and arguments[0] == 'check':
        report(*scan(arguments[1]))
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == '__main__':
    main(sys.argv[1:])
