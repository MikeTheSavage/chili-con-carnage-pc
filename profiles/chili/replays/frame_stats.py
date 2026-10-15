"""Frame-time statistics for windows of a frames.csv written by CHILI_FRAME_LOG.

usage: frame_stats.py <frames.csv> [<from>-<to>[:label] ...] [--skip-every N]

Nearest-rank percentiles of wall ms per game frame, plus guest frames per guest
second for each window. --skip-every N leaves out frames that are a multiple of
N (the ones that also wrote a PNG).
"""
import csv, math, sys


def rank(sorted_values, fraction):
    index = max(0, min(len(sorted_values) - 1, math.ceil(fraction * len(sorted_values)) - 1))
    return sorted_values[index]


def main(argv):
    skip = 0
    if '--skip-every' in argv:
        at = argv.index('--skip-every')
        skip = int(argv[at + 1])
        del argv[at:at + 2]
    rows = list(csv.DictReader(open(argv[1], newline='')))
    windows = argv[2:] or ['1-%d:all' % len(rows)]
    for window in windows:
        span, _, label = window.partition(':')
        first, last = (int(v) for v in span.split('-'))
        chosen = [r for r in rows if first <= int(r['frame']) <= last and not (skip and int(r['frame']) % skip == 0)]
        if not chosen:
            print('%s frames %d-%d: no rows' % (label, first, last))
            continue
        wall = sorted(float(r['wall_ms']) for r in chosen)
        guest = sum(float(r['guest_ms']) for r in chosen)
        print('%-10s frames %d-%d count=%d mean=%.1f p50=%.1f p90=%.1f p99=%.1f max=%.1f ms  guest_fps=%.2f wall_fps=%.2f guest_s=%.1f wall_s=%.1f' % (
            label or 'window', first, last, len(chosen), sum(wall) / len(wall), rank(wall, 0.5), rank(wall, 0.9),
            rank(wall, 0.99), wall[-1], len(chosen) * 1000.0 / guest if guest else 0.0,
            len(chosen) * 1000.0 / sum(wall), guest / 1000.0, sum(wall) / 1000.0))


if __name__ == '__main__':
    main(sys.argv)
