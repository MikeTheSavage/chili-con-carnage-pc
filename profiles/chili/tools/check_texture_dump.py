#!/usr/bin/env python3
"""Compares a run-time texture dump ([Mods] DumpTextures) with the extractor's index.

    python tools\\check_texture_dump.py <dump folder> <extracted folder>

For every <key>.png of the dump: is the key in index.csv, and are the pixels those of the extracted PNG.
A key that is not in the index is a texture the game made while it ran, or one drawn with a palette the
archive does not store with it.
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sample_texture_mod import read_png  # noqa: E402


def main(argv):
    if len(argv) != 3:
        raise SystemExit(__doc__)
    dump, extracted = argv[1], argv[2]
    index = {row['key']: row for row in csv.DictReader(open(os.path.join(extracted, 'index.csv'), newline='', encoding='utf-8'))}
    keys = sorted(name[:-4] for name in os.listdir(dump) if name.lower().endswith('.png'))
    known = [key for key in keys if key in index]
    print('dumped %d, in the index %d, not in the index %d' % (len(keys), len(known), len(keys) - len(known)))
    same, different, formats, sources = 0, [], {}, {}
    for key in known:
        row = index[key]
        formats[row['format']] = formats.get(row['format'], 0) + 1
        sources[row['source']] = sources.get(row['source'], 0) + 1
        mine = read_png(os.path.join(dump, key + '.png'))
        theirs = read_png(os.path.join(extracted, row['png'].replace('/', os.sep)))
        if mine == theirs:
            same += 1
        elif mine[:2] != theirs[:2]:
            different.append('%s %s: %dx%d dumped, %dx%d extracted' % ((key, row['name']) + mine[:2] + theirs[:2]))
        else:
            worst = max(abs(a - b) for a, b in zip(mine[2], theirs[2]))
            different.append('%s %s %s: largest difference %d of 255' % (key, row['format'], row['name'], worst))
    print('pixels equal %d, different %d' % (same, len(different)))
    for line in different:
        print('  ' + line)
    print('by format:', ', '.join('%s %d' % item for item in sorted(formats.items())))
    print('by source:', ', '.join('%s %d' % item for item in sorted(sources.items())))
    for key in keys:
        if key not in index:
            width, height, _ = read_png(os.path.join(dump, key + '.png'))
            print('  not in the index: %s %dx%d' % (key, width, height))
    return 1 if different else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
