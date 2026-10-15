"""Extract your own copy of the disc (.iso) into a folder; never modifies the source image."""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def extract(iso, destination):
    destination.mkdir(parents=True, exist_ok=True)
    entries = []
    with iso.open('rb') as source:
        def read(offset, count):
            source.seek(offset)
            data = source.read(count)
            if len(data) != count:
                raise ValueError('Truncated ISO')
            return data

        pvd = read(16 * 2048, 2048)
        if pvd[:7] != b'\x01CD001\x01':
            raise ValueError('Expected ISO9660 primary volume descriptor')

        def walk(record, parent):
            extent, size = struct.unpack_from('<I', record, 2)[0], struct.unpack_from('<I', record, 10)[0]
            directory = read(extent * 2048, size)
            cursor = 0
            while cursor < len(directory):
                length = directory[cursor]
                if not length:
                    cursor = (cursor // 2048 + 1) * 2048
                    continue
                item = directory[cursor:cursor + length]
                cursor += length
                if len(item) < 34:
                    raise ValueError('Invalid directory record')
                raw_name = item[33:33 + item[32]]
                if raw_name in (b'\x00', b'\x01'):
                    continue
                name = raw_name.decode('ascii').split(';')[0]
                if name in ('.', '..') or '/' in name or '\\' in name:
                    raise ValueError('Unsafe ISO filename')
                target = parent / name
                if item[25] & 2:
                    target.mkdir(exist_ok=True)
                    walk(item, target)
                    continue
                offset = struct.unpack_from('<I', item, 2)[0] * 2048
                count = struct.unpack_from('<I', item, 10)[0]
                digest = hashlib.sha256()
                with target.open('wb') as output:
                    for position in range(0, count, 1024 * 1024):
                        chunk = read(offset + position, min(1024 * 1024, count - position))
                        output.write(chunk)
                        digest.update(chunk)
                entries.append({'path': target.relative_to(destination).as_posix(), 'size': count, 'sha256': digest.hexdigest()})

        walk(pvd[156:190], destination)
    manifest = {'source': str(iso.resolve()), 'files': entries}
    (destination.parent / 'extraction_manifest.json').write_text(json.dumps(manifest, indent=2), encoding='utf-8')
    print(f'Extracted {len(entries)} files to {destination}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('iso', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    extract(args.iso, args.destination)
