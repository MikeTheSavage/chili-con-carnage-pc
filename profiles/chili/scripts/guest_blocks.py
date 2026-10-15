#!/usr/bin/env python3
"""Splits the samples inside one generated unit by guest basic block and guest function.

    python guest_blocks.py <samples file> <unit> <listing.cod> [--map <chili_recomp.map>] [--top 25]

The linker map only names the one function a generated unit compiles to. To see which
guest code inside it is hot, compile that unit again with the build's own flags plus
/FAsc (assembly listing with source line numbers and code offsets) and pass the .cod
here. The listing must come from the same flags as the sampled executable, or the code
offsets do not line up; the script checks the function size against the map.

A sample counts for the block of the unit's own frame, so time in helpers the block
calls (VFPU accessors, memory slow path) is charged to it. A function start is a block
that opens with a stack-pointer decrement; frameless leaf functions therefore merge
into the function before them.
"""
import argparse
import bisect
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import resolve_samples as rs  # noqa: E402


def load_listing(path, function, source_name):
    """Sorted (code offset, source line) for one function of a /FAsc listing, and its size."""
    offsets, lines = [], []
    inside = False
    in_source = True   # false while the listing shows code inlined from a header
    line_number = 0
    size = 0
    source = re.compile(r'^; (\d+)\s+: ')
    instruction = re.compile(r'^  ([0-9a-f]{5,8})\t([0-9a-f ]+)')
    more_bytes = re.compile(r'^\t([0-9a-f]{2}(?: [0-9a-f]{2})*)(?:\t|$)')
    with open(path, 'r', errors='replace') as handle:
        for text in handle:
            if not inside:
                inside = text.startswith(function) and ' PROC' in text
                continue
            if text.startswith(function) and ' ENDP' in text:
                break
            if text.startswith('; File '):
                in_source = text.rstrip().lower().endswith(source_name.lower())
                continue
            match = source.match(text)
            if match:
                if in_source:
                    line_number = int(match.group(1))
                continue
            match = instruction.match(text)
            if match:
                offset = int(match.group(1), 16)
                offsets.append(offset)
                lines.append(line_number)
                size = offset + len(match.group(2).split())
                continue
            match = more_bytes.match(text)
            if match:
                size += len(match.group(1).split())
    return offsets, lines, size


def load_source(path):
    """Label lines of a generated unit: (line numbers, guest addresses, starts-a-function flags)."""
    numbers, addresses, starts = [], [], []
    label = re.compile(r'^L_([0-9A-F]{8}):')
    frame = re.compile(r'ctx\.gpr\[29\] = \(ctx\.gpr\[29\] \+ static_cast<std::uint32_t>\((-\d+|429496\d+)\)\)')
    with open(path, 'r', errors='replace') as handle:
        pending = False
        for number, text in enumerate(handle, 1):
            match = label.match(text)
            if match:
                numbers.append(number)
                addresses.append(int(match.group(1), 16))
                starts.append(False)
                pending = True
            elif pending and text.strip():
                starts[-1] = bool(frame.search(text))
                pending = False
    return numbers, addresses, starts


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('samples')
    parser.add_argument('unit', help='four digits, e.g. 0004')
    parser.add_argument('listing', help='.cod written by cl /FAsc for that unit')
    parser.add_argument('--map')
    parser.add_argument('--top', type=int, default=25)
    arguments = parser.parse_args()

    info = rs.load_samples(arguments.samples)
    repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
    map_path = arguments.map
    if not map_path:
        beside_exe = os.path.splitext(info['exe'])[0] + '.map'
        beside_samples = os.path.join(os.path.dirname(os.path.abspath(arguments.samples)), os.path.basename(beside_exe.replace('\\', '/')))
        map_path = beside_samples if os.path.exists(beside_samples) else beside_exe
    resolve = rs.Resolver(info['modules'], info['exe'], map_path, info.get('exe_timestamp'))

    decorated = f'?recomp_unit_{arguments.unit}_entry@psprecomp@@'
    at = next(i for i, symbol in enumerate(resolve.symbols) if symbol[1].startswith(decorated))
    start = resolve.symbols[at][0]
    size_in_map = next(s[0] for s in resolve.symbols[at + 1:] if s[0] > start) - start
    offsets, lines, size = load_listing(arguments.listing, decorated, f'generated_unit_{arguments.unit}.cpp')
    if not offsets:
        raise SystemExit(f'{arguments.listing} has no code for unit {arguments.unit}')
    if not 0 <= size_in_map - size < 64:
        raise SystemExit(f'listing function is {size} bytes, the linked one about {size_in_map}: the listing was built with other flags')
    numbers, addresses, starts = load_source(os.path.join(repo, 'profiles', 'chili', 'analysis', 'generated', f'generated_unit_{arguments.unit}.cpp'))

    counts = collections.Counter()
    for stack, frames in info['stacks'].items():
        resolved = [resolve(address, index == 0) for index, address in enumerate(frames)]
        category, owner, _ = rs.classify(resolved)
        if category != 'unit' or owner != arguments.unit:
            continue
        index = next(i for i, f in enumerate(resolved) if rs.frame_kind(f) and rs.frame_kind(f)[0] == 'unit')
        counts[stack] = frames[index] - resolve.exe_base - (0 if index == 0 else 1) - start
    blocks = collections.Counter()
    unit_total = 0
    for frame_stacks in info['frames'].values():
        for stack in frame_stacks:
            if stack in counts:
                unit_total += 1
                where = bisect.bisect_right(offsets, counts[stack]) - 1
                at_label = bisect.bisect_right(numbers, lines[where]) - 1 if where >= 0 else -1
                blocks[at_label] += 1
    all_samples = sum(len(s) for s in info['frames'].values())

    functions = collections.Counter()
    spans = {}
    for at_label, count in blocks.items():
        head = at_label
        while head > 0 and not starts[head]:
            head -= 1
        key = addresses[head] if head >= 0 else 0
        functions[key] += count
        low, high = spans.get(key, (1 << 32, 0))
        spans[key] = (min(low, addresses[at_label] if at_label >= 0 else 0), max(high, addresses[at_label] if at_label >= 0 else 0))

    print(f'unit {arguments.unit}: {unit_total} samples, {100.0 * unit_total / all_samples:.2f}% of all; '
          f'{len(blocks)} of {len(numbers)} guest blocks sampled')
    print(f'\n== guest functions (start = block that opens a stack frame), top {arguments.top} ==')
    print(f'{"samples":>8}{"of unit":>9}{"cumul.":>8}  function at   hot blocks between')
    running = 0
    for key, count in functions.most_common(arguments.top):
        running += count
        print(f'{count:8d}{100.0 * count / unit_total:8.1f}%{100.0 * running / unit_total:7.1f}%  0x{key:08X}    0x{spans[key][0]:08X}-0x{spans[key][1]:08X}')
    print(f'\n== guest blocks, top {arguments.top} ==')
    print(f'{"samples":>8}{"of unit":>9}{"cumul.":>8}  block')
    running = 0
    for at_label, count in blocks.most_common(arguments.top):
        running += count
        name = f'0x{addresses[at_label]:08X} (source line {numbers[at_label]})' if at_label >= 0 else 'entry switch'
        print(f'{count:8d}{100.0 * count / unit_total:8.1f}%{100.0 * running / unit_total:7.1f}%  {name}')


if __name__ == '__main__':
    sys.exit(main())
