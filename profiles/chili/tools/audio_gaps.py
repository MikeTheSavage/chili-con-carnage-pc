#!/usr/bin/env python3
"""Gaps in a CHILI_AUDIO_WAV capture (16-bit stereo 44.1 kHz).

A gap is a run of all-zero frames with sound on both sides. Chopped sound shows
as many short gaps (5 to 250 ms); a long gap is usually the game being silent.

  python audio_gaps.py capture.wav [--skip 2.0] [--min-ms 3] [--from s --to s] [--list 20]
"""
import argparse, struct, sys
import numpy as np

def read_wav(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit('not a WAV file: ' + path)
    pos, rate, channels = 12, 44100, 2
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], struct.unpack('<I', data[pos + 4:pos + 8])[0]
        if tag == b'fmt ':
            channels, rate = struct.unpack('<HI', data[pos + 10:pos + 16])
        if tag == b'data':
            # A capture cut off by a kill has a zero size; take what is there.
            end = pos + 8 + size if size else len(data)
            samples = np.frombuffer(data[pos + 8:min(end, len(data)) // 2 * 2], dtype='<i2')
            return rate, samples[:len(samples) // channels * channels].reshape(-1, channels)
        pos += 8 + size + (size & 1)
    raise SystemExit('no data chunk: ' + path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('--skip', type=float, default=2.0, help='seconds at the start left out')
    ap.add_argument('--min-ms', type=float, default=3.0, help='shortest zero run that counts')
    ap.add_argument('--from', dest='start', type=float, default=None)
    ap.add_argument('--to', dest='end', type=float, default=None)
    ap.add_argument('--list', type=int, default=0, help='print the first N gaps')
    a = ap.parse_args()
    rate, pcm = read_wav(a.wav)
    total = len(pcm)
    first = int((a.start if a.start is not None else a.skip) * rate)
    last = int(a.end * rate) if a.end is not None else total
    pcm = pcm[first:last]
    silent = (pcm == 0).all(axis=1)
    # Run boundaries of the silent flag.
    edges = np.flatnonzero(np.diff(silent.astype(np.int8)))
    starts = list(edges[silent[edges + 1]] + 1) if len(edges) else []
    ends = list(edges[~silent[edges + 1]] + 1) if len(edges) else []
    if len(silent) and silent[0]: starts.insert(0, 0)
    if len(silent) and silent[-1]: ends.append(len(silent))
    runs = [(s, e - s) for s, e in zip(starts, ends)]
    inner = [(s, n) for s, n in runs if s > 0 and s + n < len(silent) and n * 1000.0 / rate >= a.min_ms]
    ms = lambda n: n * 1000.0 / rate
    short = [r for r in inner if ms(r[1]) <= 250.0]
    loud = np.abs(pcm.astype(np.int32)).max() if len(pcm) else 0
    # Clicks: the largest jump between neighbouring samples, against the level around it.
    jump = np.abs(np.diff(pcm[:, 0].astype(np.int32))).max() if len(pcm) > 1 else 0
    print(f'file={a.wav}')
    print(f'duration_s={total / rate:.3f} analysed_s={len(pcm) / rate:.3f} from_s={first / rate:.2f} peak={loud} largest_step={jump}')
    print(f'silent_total_s={silent.sum() / rate:.3f} ({100.0 * silent.mean() if len(silent) else 0:.1f}% of the analysed part)')
    print(f'gaps_inside_sound={len(inner)} total_ms={sum(ms(n) for _, n in inner):.1f} longest_ms={max([ms(n) for _, n in inner], default=0):.1f}')
    print(f'short_gaps_3_to_250_ms={len(short)} total_ms={sum(ms(n) for _, n in short):.1f} per_minute={len(short) * 60.0 * rate / max(len(pcm), 1):.1f}')
    for s, n in inner[:a.list]:
        print(f'  gap at {(first + s) / rate:9.3f} s  {ms(n):8.1f} ms')
    return 0

if __name__ == '__main__':
    sys.exit(main())
