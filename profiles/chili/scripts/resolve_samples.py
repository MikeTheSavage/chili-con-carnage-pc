#!/usr/bin/env python3
"""Turns a CHILI_SAMPLER file into shares of time per generated unit and host function.

    python resolve_samples.py <samples file> [--csv <frames.csv>] [--map <chili_recomp.map>]
                              [--top 40] [--json <file>] [--first <frame>] [--last <frame>]

The sampler (host/sampling_profiler.hpp) writes one line per distinct call stack of the
runtime thread. Addresses in the executable are resolved with the linker map that the
build writes beside it (bin/chili_recomp.map); other modules by their export tables.
A relink replaces that map, so a chili_recomp.map copied beside the samples file is
used first, and a map from another link than the sampled executable is refused.

Every sample is given to one owner, found by walking from the sampled instruction up:
  drawing    any frame under chili::GeRuntime (rasterizer: under draw_primitive)
  unit NNNN  the first generated unit on the stack; helpers it calls count for it,
             except unit-to-unit chain calls, which count as dispatch
  HLE        the first firmware import on the stack (psprecomp::import_N)
  present    chili::Presentation::tick (window pump, GDI blit, per-dispatch service tick)
  dispatch   psprecomp::Runtime::run and whatever else it calls
"""
import argparse
import bisect
import collections
import csv
import ctypes
import json
import os
import re
import struct
import sys

PREFERRED_BASE = 0x140000000


def load_map(path, timestamp=None):
    """Sorted (rva, decorated name, object) for every symbol in a linker map."""
    symbols = []
    with open(path, 'r', errors='replace') as handle:
        for line in handle:
            if timestamp is not None and line.startswith(' Timestamp is '):
                linked = int(line.split()[2], 16)
                if linked != timestamp:
                    raise SystemExit(f'{path} is from another link ({linked:08x}) than the sampled executable ({timestamp:08x}); '
                                     'pass the map written with that executable as --map')
            if len(line) < 40 or line[5:6] != ':':
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                address = int(parts[2], 16)
            except ValueError:
                continue
            # $LN labels are jump tables inside a function, not functions.
            if address >= PREFERRED_BASE and not parts[1].startswith('$LN'):
                symbols.append((address - PREFERRED_BASE, parts[1], parts[-1]))
    symbols.sort()
    return symbols


def load_exports(path):
    """Sorted (rva, name) from a DLL's export table; empty when it cannot be read."""
    try:
        with open(path, 'rb') as handle:
            data = handle.read()
        pe = struct.unpack_from('<I', data, 0x3C)[0]
        sections_count = struct.unpack_from('<H', data, pe + 6)[0]
        optional_size = struct.unpack_from('<H', data, pe + 20)[0]
        optional = pe + 24
        if struct.unpack_from('<H', data, optional)[0] != 0x20B:
            return []
        export_rva = struct.unpack_from('<I', data, optional + 112)[0]
        sections = []
        for i in range(sections_count):
            entry = optional + optional_size + 40 * i
            virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from('<IIII', data, entry + 8)
            sections.append((virtual_address, max(virtual_size, raw_size), raw_offset))

        def offset(rva):
            for virtual_address, size, raw_offset in sections:
                if virtual_address <= rva < virtual_address + size:
                    return rva - virtual_address + raw_offset
            raise ValueError(rva)

        if not export_rva:
            return []
        directory = offset(export_rva)
        names_count = struct.unpack_from('<I', data, directory + 24)[0]
        functions, names, ordinals = struct.unpack_from('<III', data, directory + 28)
        functions, names, ordinals = offset(functions), offset(names), offset(ordinals)
        exports = []
        for i in range(names_count):
            name_offset = offset(struct.unpack_from('<I', data, names + 4 * i)[0])
            name = data[name_offset:data.index(b'\0', name_offset)].decode('ascii', 'replace')
            ordinal = struct.unpack_from('<H', data, ordinals + 2 * i)[0]
            exports.append((struct.unpack_from('<I', data, functions + 4 * ordinal)[0], name))
        exports.sort()
        return exports
    except (OSError, ValueError, struct.error):
        return []


class Undecorator:
    def __init__(self):
        self.cache = {}
        try:
            self.function = ctypes.WinDLL('dbghelp').UnDecorateSymbolName
            self.function.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32]
            self.function.restype = ctypes.c_uint32
        except (OSError, AttributeError):
            self.function = None

    def __call__(self, name):
        if name not in self.cache:
            text = name
            if self.function and name.startswith('?'):
                buffer = ctypes.create_string_buffer(8192)
                if self.function(name.encode('ascii', 'replace'), buffer, len(buffer), 0x1000):  # UNDNAME_NAME_ONLY
                    text = buffer.value.decode('ascii', 'replace')
            self.cache[name] = text
        return self.cache[name]


class Resolver:
    """address -> (module, function, object)."""

    def __init__(self, modules, exe_path, map_path, timestamp=None):
        self.modules = sorted(modules)
        self.bases = [m[0] for m in self.modules]
        self.exe_base = next(base for base, _, path in self.modules if os.path.normcase(path) == os.path.normcase(exe_path))
        self.symbols = load_map(map_path, timestamp)
        self.symbol_rvas = [s[0] for s in self.symbols]
        self.undecorate = Undecorator()
        self.exports = {}
        self.cache = {}

    def __call__(self, address, leaf):
        key = (address, leaf)
        found = self.cache.get(key)
        if found is None:
            found = self.cache[key] = self.lookup(address, leaf)
        return found

    def lookup(self, address, leaf):
        index = bisect.bisect_right(self.bases, address) - 1
        if index < 0 or address >= self.modules[index][0] + self.modules[index][1]:
            return ('?', hex(address), '')
        base, _, path = self.modules[index]
        # A return address is the instruction after the call; step back into the call.
        rva = address - base - (0 if leaf else 1)
        if base == self.exe_base:
            at = bisect.bisect_right(self.symbol_rvas, rva) - 1
            if at < 0:
                return ('exe', hex(rva), '')
            return ('exe', self.undecorate(self.symbols[at][1]), self.symbols[at][2])
        name = os.path.basename(path).lower()
        if path not in self.exports:
            exports = load_exports(path)
            self.exports[path] = (exports, [e[0] for e in exports])
        exports, rvas = self.exports[path]
        at = bisect.bisect_right(rvas, rva) - 1
        # Internal functions have no export; the nearest one below is only a hint.
        return (name, exports[at][1] if at >= 0 else hex(rva), '')


# An outlined unit has one C++ function per guest function: recomp_unit_NNNN_fn_<guest address>.
UNIT = re.compile(r'^psprecomp::recomp_unit_(\d{4})(?:_entry|_fn_[0-9A-F]{8})?$')
GUEST_FUNCTION = re.compile(r'^psprecomp::recomp_unit_(\d{4})_fn_([0-9A-F]{8})$')
IMPORT = re.compile(r'^psprecomp::import_(\d+)$')
CHAIN = ('invoke_chained', 'DepthGuard', 'psprecomp::Runtime::')


def frame_kind(frame):
    module, name, _ = frame
    if module != 'exe':
        return None
    match = UNIT.match(name)
    if match:
        return ('unit', match.group(1))
    match = IMPORT.match(name)
    if match:
        return ('import', int(match.group(1)))
    if 'chili::GeRuntime::draw_primitive' in name:
        return ('draw', None)
    if 'chili::GeRuntime' in name:
        return ('ge', None)
    if name.startswith('chili::Presentation::'):
        return ('present', None)
    if name == 'psprecomp::Runtime::run':
        return ('run', None)
    return None


def label(frame):
    module, name, _ = frame
    return name if module == 'exe' else f'{module}!{name}'


def classify(frames):
    """(category, owner, detail) for one resolved stack, leaf first."""
    kinds = [frame_kind(f) for f in frames]
    if any(k and k[0] == 'draw' for k in kinds):
        return ('draw', 'rasterizer (GeRuntime::draw_primitive)', label(frames[0]))
    if any(k and k[0] == 'ge' for k in kinds):
        return ('draw', 'GE list decode and state (GeRuntime, outside draws)', label(frames[0]))
    for index, kind in enumerate(kinds):
        if not kind:
            continue
        if kind[0] == 'unit':
            if index == 0:
                return ('unit', kind[1], 'self')
            leaf = label(frames[0])
            if any(text in leaf for text in CHAIN) or any(any(text in label(f) for text in CHAIN) for f in frames[1:index]):
                return ('dispatch', 'unit-to-unit chain call', leaf)
            if 'GuestMemory::' in leaf:
                return ('unit', kind[1], 'memory slow path')
            return ('unit', kind[1], 'helper: ' + leaf)
        if kind[0] == 'import':
            return ('hle', kind[1], label(frames[0]))
        if kind[0] == 'present':
            return ('present', 'chili::Presentation::tick', label(frames[0]))
        if kind[0] == 'run':
            return ('dispatch', label(frames[index - 1]) if index else 'psprecomp::Runtime::run', label(frames[0]))
    return ('other', label(frames[0]), label(frames[0]))


def load_samples(path):
    info = {'modules': [], 'stacks': {}, 'frames': {}}
    with open(path, 'r') as handle:
        if not handle.readline().startswith('chili-sampler 1'):
            raise SystemExit(f'{path} is not a sampler file')
        for line in handle:
            kind, _, rest = line.rstrip('\n').partition(' ')
            if kind == 'stack':
                parts = rest.split()
                info['stacks'][int(parts[0])] = [int(a, 16) for a in parts[2:]]
            elif kind == 'frame':
                parts = rest.split()
                info['frames'][int(parts[0])] = [int(s) for s in parts[1:]]
            elif kind == 'module':
                base, size, name = rest.split(' ', 2)
                info['modules'].append((int(base, 16), int(size, 16), name))
            elif kind == 'exe':
                info['exe'] = rest
            elif kind == 'exe_timestamp':
                info['exe_timestamp'] = int(rest, 16)
            elif kind == 'hz':
                info['hz'] = int(rest)
            elif kind == 'samples':
                parts = rest.split()
                info['samples'] = int(parts[0])
                info.update({parts[i]: float(parts[i + 1]) for i in range(1, len(parts) - 1, 2)})
    return info


def import_names(repo):
    """import index -> 'module::function' from the generated registry and the NID table."""
    nids = {}
    try:
        with open(os.path.join(repo, 'src', 'nid_registry.cpp'), 'r', errors='replace') as handle:
            for module, nid, name in re.findall(r'add\("([^"]+)",\s*0x([0-9A-Fa-f]+)u?,\s*"([^"]+)"\)', handle.read()):
                nids[(module, int(nid, 16))] = name
    except OSError:
        pass
    names = {}
    registry = os.path.join(repo, 'profiles', 'chili', 'analysis', 'generated', 'generated_registry.cpp')
    try:
        with open(registry, 'r', errors='replace') as handle:
            for index, module, nid in re.findall(r'invoke_import_cached\((\d+)u,\s*"([^"]+)",\s*0x([0-9A-Fa-f]+)u', handle.read()):
                nid = int(nid, 16)
                names[int(index)] = f'{module}::{nids.get((module, nid), "0x%08X" % nid)}'
    except OSError:
        pass
    return names


def unit_facts(repo, unit):
    """Source size, line count, lines mentioning vfpu and guest address range of a generated unit."""
    path = os.path.join(repo, 'profiles', 'chili', 'analysis', 'generated', f'generated_unit_{unit}.cpp')
    try:
        size = os.path.getsize(path)
        lines = vfpu = 0
        guest = ''
        with open(path, 'r', errors='replace') as handle:
            for line in handle:
                lines += 1
                if 'vfpu' in line.lower():
                    vfpu += 1
                if not guest and 'register_generated_unit(' in line:
                    match = re.search(r'0x([0-9A-Fa-f]{8})u,\s*(\d+)u', line)
                    if match:
                        start = int(match.group(1), 16)
                        guest = '%08X-%08X' % (start, start + int(match.group(2)))
        return {'source_kb': round(size / 1024), 'lines': lines, 'vfpu_lines': vfpu, 'guest': guest}
    except OSError:
        return {}


def percentile(values, fraction):
    if not values:
        return 0.0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, int(-(-fraction * len(ordered) // 1)) - 1))]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('samples')
    parser.add_argument('--map', help='linker map (default: beside the sampled executable)')
    parser.add_argument('--csv', help='frames.csv of the same run, for milliseconds per frame')
    parser.add_argument('--top', type=int, default=40)
    parser.add_argument('--json', help='also write every table to this file')
    parser.add_argument('--first', type=int, default=0, help='first game frame to count')
    parser.add_argument('--last', type=int, default=1 << 62, help='last game frame to count')
    parser.add_argument('--symbols', help='symbols.csv (address,name,...) from profiles/chili/symbols: names the guest functions')
    arguments = parser.parse_args()

    info = load_samples(arguments.samples)
    repo = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
    map_path = arguments.map
    if not map_path:
        beside_exe = os.path.splitext(info['exe'])[0] + '.map'
        beside_samples = os.path.join(os.path.dirname(os.path.abspath(arguments.samples)), os.path.basename(beside_exe.replace('\\', '/')))
        map_path = beside_samples if os.path.exists(beside_samples) else beside_exe
    resolve = Resolver(info['modules'], info['exe'], map_path, info.get('exe_timestamp'))
    imports = import_names(repo)

    owners = {}
    guest_functions = {}
    for stack, addresses in info['stacks'].items():
        frames = [resolve(address, index == 0) for index, address in enumerate(addresses)]
        category, owner, detail = classify(frames)
        if category == 'hle':
            owner = imports.get(owner, f'import_{owner}')
        owners[stack] = (category, owner, detail, label(frames[0]))
        if category == 'unit':
            # The innermost generated frame; in an outlined unit it names the guest function.
            inner = next((f[1] for f in frames if f[0] == 'exe' and UNIT.match(f[1])), '')
            match = GUEST_FUNCTION.match(inner)
            guest_functions[stack] = (match.group(1), match.group(2)) if match else None

    frames_used = sorted(f for f in info['frames'] if arguments.first <= f <= arguments.last)
    totals = collections.Counter()      # category
    by_owner = collections.Counter()    # (category, owner)
    by_detail = collections.Counter()   # (category, owner, detail)
    by_leaf = collections.Counter()     # (category, leaf)
    by_function = collections.Counter() # (unit, guest address) of outlined units
    per_frame = {}
    for frame in frames_used:
        drawing = present = 0
        for stack in info['frames'][frame]:
            category, owner, detail, leaf = owners[stack]
            totals[category] += 1
            by_owner[(category, owner)] += 1
            by_detail[(category, owner, detail)] += 1
            by_leaf[(category, leaf)] += 1
            if guest_functions.get(stack):
                by_function[guest_functions[stack]] += 1
            drawing += category == 'draw'
            present += category == 'present'
        per_frame[frame] = (len(info['frames'][frame]), drawing, present)
    total = sum(totals.values())
    if not total:
        raise SystemExit('no samples in the chosen frame range')
    non_draw = total - totals['draw']
    guest = totals['unit']

    wall = {}
    if arguments.csv:
        with open(arguments.csv, newline='') as handle:
            for row in csv.DictReader(handle):
                wall[int(row['frame'])] = float(row['wall_ms'])
    timed = [f for f in frames_used if f in wall]
    if timed:
        frame_ms = sum(wall[f] for f in timed) / len(timed)
        non_draw_ms = [wall[f] * (per_frame[f][0] - per_frame[f][1]) / per_frame[f][0] for f in timed]
        # Game code and firmware services only: what is left without drawing and without the window.
        game_ms = [wall[f] * (per_frame[f][0] - per_frame[f][1] - per_frame[f][2]) / per_frame[f][0] for f in timed]
    else:
        frame_ms = info.get('window_ms', 0.0) / max(1, len(frames_used))
        non_draw_ms = game_ms = []

    def ms(count):
        """Mean milliseconds per game frame that this many samples stand for."""
        return frame_ms * count / total

    out = []
    emit = out.append
    emit(f'samples {total} over game frames {frames_used[0]}-{frames_used[-1]} ({len(frames_used)} frames), '
         f'{total / len(frames_used):.1f} samples per frame')
    emit(f'sampler: {info.get("hz")} Hz asked, {1000.0 * info["samples"] / max(1.0, info.get("window_ms", 0.0)):.0f} Hz got; '
         f'target held suspended {100.0 * info.get("suspended_ms", 0.0) / max(1.0, info.get("window_ms", 0.0)):.1f}% of the time; '
         f'{int(info.get("truncated", 0))} stacks not walked to the end')
    emit(f'wall ms per game frame: {frame_ms:.1f}' + (' (frames.csv)' if timed else ' (sampler window / frames)'))
    emit('')
    emit('== Where the runtime thread is (share of all samples) ==')
    rows = [('a', 'drawing: software rasterizer', by_owner[('draw', 'rasterizer (GeRuntime::draw_primitive)')]),
            ('a', 'drawing: GE list decode and state', by_owner[('draw', 'GE list decode and state (GeRuntime, outside draws)')]),
            ('b', 'generated units (recompiled game code)', totals['unit']),
            ('c', 'firmware services (HLE imports, not GE)', totals['hle']),
            ('c', 'Presentation::tick (GDI blit, window pump, service tick)', totals['present']),
            ('d', 'scheduler and dispatch (Runtime::run, chain calls)', totals['dispatch']),
            ('e', 'everything else (stack not resolved)', totals['other'])]
    emit(f'{"":3}{"":58}{"samples":>9}{"share":>8}{"ms/frame":>10}{"of non-draw":>13}')
    for tag, name, count in rows:
        share = '' if name.startswith('drawing') else f'{100.0 * count / non_draw:12.1f}%'
        emit(f'({tag}) {name:57}{count:9d}{100.0 * count / total:7.1f}%{ms(count):10.2f}{share}')
    emit(f'    {"all drawing":57}{totals["draw"]:9d}{100.0 * totals["draw"] / total:7.1f}%{ms(totals["draw"]):10.2f}')
    emit(f'    {"all non-drawing":57}{non_draw:9d}{100.0 * non_draw / total:7.1f}%{ms(non_draw):10.2f}')

    def spread(values):
        if not values:
            return None
        return {'mean': sum(values) / len(values), 'p50': percentile(values, 0.5), 'p90': percentile(values, 0.9),
                'p99': percentile(values, 0.99), 'max': max(values)}

    for name, values in (('non-drawing', non_draw_ms), ('game code + services (non-drawing without Presentation::tick)', game_ms)):
        if values:
            emit(f'{name} ms per frame, frame by frame: ' + '  '.join(f'{key} {value:.2f}' for key, value in spread(values).items()))

    units = sorted(((count, owner) for (category, owner), count in by_owner.items() if category == 'unit'), reverse=True)
    emit('')
    emit(f'== Generated units, top {min(arguments.top, len(units))} of {len(units)} with samples ==')
    emit(f'{"unit":6}{"guest range":19}{"samples":>8}{"of all":>8}{"non-draw":>9}{"of guest":>9}{"cumul.":>8}{"ms/frame":>9}'
         f'{"self":>7}{"mem slow":>9}{"helpers":>8}{"src KB":>8}{"vfpu lines":>11}')
    unit_table = []
    running = 0
    coverage = {}
    for rank, (count, unit) in enumerate(units, 1):
        running += count
        for goal in (50, 80, 90, 95):
            if goal not in coverage and running * 100.0 >= goal * guest:
                coverage[goal] = rank
        own = by_detail[('unit', unit, 'self')]
        slow = by_detail[('unit', unit, 'memory slow path')]
        entry = {'unit': unit, 'samples': count, 'share_all': count / total, 'share_non_draw': count / non_draw,
                 'share_guest': count / guest, 'cumulative_guest': running / guest, 'ms_per_frame': ms(count),
                 'self': own, 'memory_slow_path': slow, 'helpers': count - own - slow}
        if rank <= arguments.top:
            entry.update(unit_facts(repo, unit))
            emit(f'{unit:6}{entry.get("guest", ""):19}{count:8d}{100.0 * count / total:7.2f}%{100.0 * count / non_draw:8.2f}%'
                 f'{100.0 * count / guest:8.2f}%{100.0 * running / guest:7.1f}%{ms(count):9.3f}'
                 f'{100.0 * own / count:6.0f}%{100.0 * slow / count:8.0f}%{100.0 * (count - own - slow) / count:7.0f}%'
                 f'{entry.get("source_kb", 0):8d}{entry.get("vfpu_lines", 0):11d}')
        unit_table.append(entry)
    emit('units needed to cover guest-code time: ' + ', '.join(f'{goal}% = {coverage[goal]}' for goal in sorted(coverage)))
    helper_leaves = collections.Counter()
    for (category, owner, detail), count in by_detail.items():
        if category == 'unit':
            helper_leaves['inside the unit function' if detail == 'self' else detail] += count
    emit('guest-code samples by where the instruction pointer was:')
    for name, count in helper_leaves.most_common(12):
        emit(f'  {count:8d} {100.0 * count / guest:6.2f}% of guest  {name[:110]}')
    slow_leaves = collections.Counter({leaf: count for (category, leaf), count in by_leaf.items() if category == 'unit' and 'GuestMemory::' in leaf})
    if slow_leaves:
        emit('memory slow path by function: ' + ', '.join(f'{name.split("::")[-1]} {count}' for name, count in slow_leaves.most_common(8)))

    # Engine names (Total Overdose names mapped onto this build) for the guest function table.
    symbol_addresses, symbol_names = [], []
    if arguments.symbols:
        with open(arguments.symbols, newline='', encoding='utf-8', errors='replace') as handle:
            named = sorted((int(row['address'], 16), row['name']) for row in csv.DictReader(handle)
                           if row.get('kind') == 'function' and row.get('address'))
        symbol_addresses = [address for address, _ in named]
        symbol_names = [name for _, name in named]

    def guest_name(address):
        """The symbol at a guest address, or the nearest one below it in the same 16 KiB as a hint."""
        at = bisect.bisect_right(symbol_addresses, address) - 1
        if at < 0:
            return ''
        if symbol_addresses[at] == address:
            return symbol_names[at]
        if address - symbol_addresses[at] < 0x4000:
            return f'(after {symbol_names[at]} +0x{address - symbol_addresses[at]:X})'
        return ''

    function_table = []
    if by_function:
        outlined = sum(by_function.values())
        emit('')
        emit(f'== Guest functions of outlined units, top {min(arguments.top, len(by_function))} of {len(by_function)} '
             f'({100.0 * outlined / guest:.1f}% of guest-code samples are in outlined functions) ==')
        emit(f'{"function":12}{"unit":6}{"samples":>8}{"of guest":>9}{"cumul.":>8}{"ms/frame":>9}')
        running = 0
        for rank, ((unit, address), count) in enumerate(by_function.most_common(), 1):
            running += count
            if rank <= arguments.top:
                emit(f'0x{address}  {unit:6}{count:8d}{100.0 * count / guest:8.2f}%{100.0 * running / guest:7.1f}%{ms(count):9.3f}'
                     f'  {guest_name(int(address, 16))}')
            function_table.append({'address': '0x' + address, 'unit': unit, 'samples': count, 'ms_per_frame': ms(count),
                                   'name': guest_name(int(address, 16))})

    def table(title, category, limit):
        rows = sorted(((count, owner) for (c, owner), count in by_owner.items() if c == category), reverse=True)
        if not rows:
            return []
        emit('')
        emit(f'== {title} ==')
        emit(f'{"samples":>8}{"of all":>8}{"non-draw":>9}{"ms/frame":>9}  owner  [top leaf functions]')
        result = []
        for count, owner in rows[:limit]:
            leaves = sorted(((n, d) for (c, o, d), n in by_detail.items() if c == category and o == owner), reverse=True)[:3]
            emit(f'{count:8d}{100.0 * count / total:7.2f}%{100.0 * count / non_draw:8.2f}%{ms(count):9.3f}  {str(owner)[:70]}  '
                 f'[{"; ".join(f"{d[:48]} {100.0 * n / count:.0f}%" for n, d in leaves)}]')
            result.append({'owner': owner, 'samples': count, 'share_all': count / total, 'share_non_draw': count / non_draw,
                           'ms_per_frame': ms(count), 'leaves': [{'function': d, 'samples': n} for n, d in leaves]})
        return result

    hle_table = table('Firmware services (HLE), by import', 'hle', arguments.top)
    present_table = table('Presentation', 'present', 5)
    dispatch_table = table('Scheduler and dispatch, by the function Runtime::run called', 'dispatch', 20)
    other_table = table('Everything else, by sampled function', 'other', 10)

    emit('')
    emit('== Host functions outside drawing and outside unit functions, by sampled function ==')
    emit(f'{"samples":>8}{"of all":>8}{"non-draw":>9}{"ms/frame":>9}  function (owner kind)')
    host_leaves = collections.Counter({key: count for key, count in by_leaf.items()
                                       if key[0] != 'draw' and not UNIT.match(key[1])})
    host_table = []
    for (category, leaf), count in host_leaves.most_common(arguments.top):
        emit(f'{count:8d}{100.0 * count / total:7.2f}%{100.0 * count / non_draw:8.2f}%{ms(count):9.3f}  {leaf[:110]} ({category})')
        host_table.append({'function': leaf, 'kind': category, 'samples': count, 'share_non_draw': count / non_draw})

    emit('')
    emit(f'== Non-drawing time ranked by owner, top {arguments.top} ==')
    emit(f'{"samples":>8}{"non-draw":>9}{"cumul.":>8}{"ms/frame":>9}  owner')
    ranking = sorted(((count, category, owner) for (category, owner), count in by_owner.items() if category != 'draw'), reverse=True)
    running = 0
    ranking_table = []
    for count, category, owner in ranking[:arguments.top]:
        running += count
        name = f'unit {owner}' if category == 'unit' else f'{category}: {owner}'
        emit(f'{count:8d}{100.0 * count / non_draw:8.2f}%{100.0 * running / non_draw:7.1f}%{ms(count):9.3f}  {name[:100]}')
        ranking_table.append({'owner': name, 'samples': count, 'share_non_draw': count / non_draw, 'cumulative': running / non_draw})

    emit('')
    emit('== Drawing, by sampled function (top 15) ==')
    for (category, leaf), count in collections.Counter({k: c for k, c in by_leaf.items() if k[0] == 'draw'}).most_common(15):
        emit(f'{count:8d}{100.0 * count / total:7.2f}%  {leaf[:120]}')

    print('\n'.join(out))
    if arguments.json:
        summary = {
            'samples': total, 'frames': [frames_used[0], frames_used[-1]], 'frame_count': len(frames_used),
            'wall_ms_per_frame': frame_ms, 'sampler': {k: info.get(k) for k in ('hz', 'window_ms', 'suspended_ms', 'truncated', 'failed')},
            'categories': {name: {'samples': count, 'share': count / total, 'ms_per_frame': ms(count)} for _, name, count in rows},
            'drawing_ms_per_frame': ms(totals['draw']), 'non_drawing_ms_per_frame': ms(non_draw),
            'non_drawing_ms_per_frame_distribution': spread(non_draw_ms),
            'game_code_and_services_ms_per_frame_distribution': spread(game_ms),
            'units_to_cover_guest_time': coverage, 'units': unit_table, 'guest_functions': function_table, 'hle': hle_table, 'presentation': present_table,
            'dispatch': dispatch_table, 'other': other_table, 'host_functions': host_table, 'non_drawing_ranking': ranking_table,
        }
        with open(arguments.json, 'w') as handle:
            json.dump(summary, handle, indent=1)


if __name__ == '__main__':
    sys.exit(main())
