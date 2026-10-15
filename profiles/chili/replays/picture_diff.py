"""Compares two frame pictures of the same size, pixel by pixel.

usage: picture_diff.py <a.png> <b.png> [<name>=<x0>,<y0>,<x1>,<y1> ...]

For the whole picture and for each rectangle (given in PSP pixels, 0..480 by 0..272, whatever the pictures'
size): the number of pixels, how many differ at all, by more than 2, 8 and 32 levels of 255 in any channel,
and the largest difference. Exit code 0 when the pictures are equal, 1 when they differ, 2 for pictures of
different sizes.

Made for the overlay pass (RENDERING.md, "The overlay pass"): a frame picture of a screen of the game against
the same frame with CHILI_OVERLAY_TEST=echo, which draws that screen again over itself. The pictures the game
writes are read with the reader of tools\\sample_texture_mod.py, which is plain Python: fine at internal scale
1, slow for a 3840x2176 picture.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from sample_texture_mod import read_png  # noqa: E402


def measure(a, b, width, box):
    x0, y0, x1, y1 = box
    count = differ = over2 = over8 = over32 = largest = 0
    for y in range(y0, y1):
        row = y * width * 4
        for x in range(x0, x1):
            at = row + x * 4
            delta = max(abs(a[at] - b[at]), abs(a[at + 1] - b[at + 1]), abs(a[at + 2] - b[at + 2]))
            count += 1
            if delta:
                differ += 1
                over2 += delta > 2
                over8 += delta > 8
                over32 += delta > 32
                largest = max(largest, delta)
    return count, differ, over2, over8, over32, largest


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__)
    width, height, a = read_png(argv[1])
    other_width, other_height, b = read_png(argv[2])
    if (width, height) != (other_width, other_height):
        print('%dx%d against %dx%d: not the same size' % (width, height, other_width, other_height))
        return 2
    scale = width / 480.0
    regions = [('whole picture', (0, 0, width, height))]
    for item in argv[3:]:
        name, _, numbers = item.partition('=')
        x0, y0, x1, y1 = (float(part) for part in numbers.split(','))
        regions.append((name, (max(0, int(x0 * scale)), max(0, int(y0 * scale)), min(width, int(x1 * scale)), min(height, int(y1 * scale)))))
    print('%-28s %8s %8s %8s %8s %8s %8s' % ('', 'pixels', 'differ', '>2', '>8', '>32', 'largest'))
    equal = True
    for name, box in regions:
        result = measure(a, b, width, box)
        print('%-28s %8d %8d %8d %8d %8d %8d' % ((name,) + result))
        if name == 'whole picture':
            equal = result[1] == 0
    return 0 if equal else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
