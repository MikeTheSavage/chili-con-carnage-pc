"""Checks that a BOOT.BIN is the game executable this port is made for.

    py -3 check_boot.py <disc folder>/PSP_GAME/SYSDIR/BOOT.BIN

Chili Con Carnage, USA release (ULUS-10216): 8,616,992 bytes, FNV-1a 64 0xA18A6D5249286C4A. The game program
makes the same check when it starts (host/launcher.hpp). Exit code 0 when it matches, 1 when it does not.
"""
import sys

EXPECTED_SIZE = 8616992
EXPECTED_FNV = 0xA18A6D5249286C4A


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        return 2
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    found = fnv1a64(data)
    if len(data) == EXPECTED_SIZE and found == EXPECTED_FNV:
        print("BOOT.BIN: Chili Con Carnage (USA, ULUS-10216)")
        return 0
    print("BOOT.BIN is not the one of Chili Con Carnage (USA, ULUS-10216): %d bytes, FNV-1a 64 %016X" % (len(data), found))
    return 1


if __name__ == "__main__":
    sys.exit(main())
