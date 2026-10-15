#!/usr/bin/env python3
"""Compare a sound the game produced with a reference decode of the same asset, in numbers.

  speech_compare.py <reference.wav> <game.wav> [--from SECONDS] [--to SECONDS] [--json]

<reference.wav> is the asset decoded apart from the game (tools/at3_reference.cpp, one or two
channels). <game.wav> is 16-bit stereo: a stream tap (CHILI_ATRAC_TAP, what the game was handed
for that stream) or a capture of the output (CHILI_AUDIO_WAV); --from/--to cut a window out of it.

It prints, for the game's sound against the reference:

  lag           where the reference fits best in the game's sound (cross-correlation of the two
                mono sums), in samples and milliseconds
  correlation   normalised correlation at that lag: 1.000 is the same waveform, 0 unrelated
  gain          the level of the game's sound relative to the reference (least squares)
  snr           reference power over the power of what is left after removing gain x reference
                from the game's sound, in dB; identical gives "exact" (every sample equal)
  centroid      spectral centroid of both: a sound played too fast has it higher
  pitch         median fundamental of the voiced parts of both and their ratio
  duration      seconds of sound above the noise floor in both
  left/right    correlation between the game's two channels
  mono-as-stereo  how many 2048-sample blocks of the game's sound are the reference's block written as
                one channel into a two-channel buffer (left = even samples, right = odd samples, in
                the first half of the block): the signature of the fault fixed in host/atrac_stream.cpp

Needs numpy.
"""
import argparse
import json
import struct
import sys

import numpy as np

RATE = 44100
BLOCK = 2048


def read_wav(path):
    """(samples as float64 array of shape [frames, channels], rate) of a 16-bit PCM WAV."""
    with open(path, 'rb') as handle:
        data = handle.read()
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise SystemExit('%s is not a WAV file' % path)
    at, channels, rate, pcm = 12, 0, 0, None
    while at + 8 <= len(data):
        tag, length = data[at:at + 4], struct.unpack_from('<I', data, at + 4)[0]
        if tag == b'fmt ':
            code, channels, rate, _, _, bits = struct.unpack_from('<HHIIHH', data, at + 8)
            if code != 1 or bits != 16:
                raise SystemExit('%s is not 16-bit PCM' % path)
        elif tag == b'data':
            pcm = data[at + 8:at + 8 + length]
            break
        at += 8 + ((length + 1) & ~1)
    if pcm is None or not channels:
        raise SystemExit('%s has no samples' % path)
    frames = len(pcm) // (2 * channels)
    return np.frombuffer(pcm[:frames * 2 * channels], dtype='<i2').astype(np.float64).reshape(frames, channels), rate


def best_lag(game, reference):
    """Lag of `reference` inside `game` that maximises the cross-correlation, and the normalised value there."""
    size = 1 << int(np.ceil(np.log2(len(game) + len(reference))))
    spectrum = np.fft.rfft(game, size) * np.conj(np.fft.rfft(reference, size))
    correlation = np.fft.irfft(spectrum, size)
    # Lags from -(len(reference) - 1) to len(game) - 1.
    lags = np.concatenate((np.arange(0, len(game)), np.arange(-(size - len(game)), 0)))
    valid = (lags > -len(reference)) & (lags < len(game))
    lag = int(lags[valid][np.argmax(correlation[:size][valid])])
    start_game, start_reference = max(lag, 0), max(-lag, 0)
    count = min(len(game) - start_game, len(reference) - start_reference)
    a, b = game[start_game:start_game + count], reference[start_reference:start_reference + count]
    denominator = np.sqrt(np.dot(a, a) * np.dot(b, b))
    return lag, (float(np.dot(a, b) / denominator) if denominator > 0 else 0.0), a, b


def centroid(signal):
    if len(signal) < 256 or not np.any(signal):
        return 0.0
    spectrum = np.abs(np.fft.rfft(signal * np.hanning(len(signal)))) ** 2
    frequencies = np.fft.rfftfreq(len(signal), 1.0 / RATE)
    return float(np.sum(frequencies * spectrum) / np.sum(spectrum))


def median_pitch(signal):
    """Median fundamental (Hz) over the voiced 46 ms frames, by autocorrelation between 70 and 500 Hz; 0 when none."""
    low, high, found = RATE // 500, RATE // 70, []
    peak = np.max(np.abs(signal)) if len(signal) else 0.0
    for start in range(0, len(signal) - BLOCK, BLOCK // 2):
        frame = signal[start:start + BLOCK]
        if np.sqrt(np.mean(frame * frame)) < 0.05 * peak:
            continue
        frame = frame - np.mean(frame)
        size = 2 * BLOCK
        auto = np.fft.irfft(np.abs(np.fft.rfft(frame, size)) ** 2, size)[:high + 1]
        if auto[0] <= 0:
            continue
        lag = low + int(np.argmax(auto[low:high + 1]))
        if auto[lag] / auto[0] > 0.5:
            found.append(RATE / lag)
    return float(np.median(found)) if found else 0.0


def active_seconds(signal):
    """Seconds of 10 ms pieces whose level is within 40 dB of the loudest piece."""
    piece = RATE // 100
    count = len(signal) // piece
    if not count:
        return 0.0
    levels = np.sqrt(np.mean(signal[:count * piece].reshape(count, piece) ** 2, axis=1))
    return float(np.sum(levels > np.max(levels) * 0.01)) * piece / RATE


def mono_as_stereo_blocks(game, reference):
    """Blocks of the game's stereo sound that hold a reference block as interleaved mono in their first half.

    A stream's first block is short (the decoder's lead-in is cut from it), so its length is found first:
    it is twice the number of leading frames whose left and right are the reference's even and odd samples."""
    size = min(len(game), len(reference))
    limit = min(size // 2, BLOCK // 2)
    match = (game[:limit, 0] == reference[0:2 * limit:2]) & (game[:limit, 1] == reference[1:2 * limit:2])
    first = limit if match.all() else int(np.argmin(match))
    blocks = 1 + max(size - 2 * first, 0) // BLOCK if first else (size + BLOCK - 1) // BLOCK
    if not first:
        return 0, blocks
    hits, at = 1, 2 * first
    while at + BLOCK <= size:
        want, have = reference[at:at + BLOCK], game[at:at + BLOCK // 2]
        if np.array_equal(have[:, 0], want[0::2]) and np.array_equal(have[:, 1], want[1::2]):
            hits += 1
        at += BLOCK
    return hits, blocks


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('reference')
    parser.add_argument('game')
    parser.add_argument('--from', dest='start', type=float, default=0.0)
    parser.add_argument('--to', dest='stop', type=float, default=0.0)
    parser.add_argument('--json', action='store_true')
    arguments = parser.parse_args()

    reference, reference_rate = read_wav(arguments.reference)
    game, game_rate = read_wav(arguments.game)
    if reference_rate != RATE or game_rate != RATE:
        raise SystemExit('both files must be 44.1 kHz')
    if game.shape[1] != 2:
        raise SystemExit('the game file must be stereo')
    first = int(arguments.start * RATE)
    last = int(arguments.stop * RATE) if arguments.stop > 0 else len(game)
    game = game[first:last]
    reference_mono = reference.mean(axis=1)
    game_mono = game.mean(axis=1)
    if not len(game_mono) or not len(reference_mono):
        raise SystemExit('nothing to compare')

    lag, correlation, a, b = best_lag(game_mono, reference_mono)
    gain = float(np.dot(a, b) / np.dot(b, b)) if np.dot(b, b) > 0 else 0.0
    rest = a - gain * b
    exact = bool(len(a) == len(b) and np.array_equal(a, b))
    noise = float(np.dot(rest, rest))
    snr = float('inf') if noise == 0 else 10.0 * np.log10(np.dot(gain * b, gain * b) / noise) if gain else float('-inf')
    left, right = game[:, 0], game[:, 1]
    channels = float(np.dot(left, right) / np.sqrt(np.dot(left, left) * np.dot(right, right))) if np.any(left) and np.any(right) else 0.0
    hits, blocks = mono_as_stereo_blocks(game, reference_mono) if reference.shape[1] == 1 else (0, 0)
    game_pitch, reference_pitch = median_pitch(a), median_pitch(b)
    result = {
        'reference_seconds': len(reference_mono) / RATE, 'game_seconds': len(game_mono) / RATE,
        'lag_samples': lag, 'lag_ms': lag * 1000.0 / RATE, 'correlation': correlation, 'gain': gain,
        'snr_db': None if snr in (float('inf'), float('-inf')) else snr, 'exact': exact,
        'centroid_reference_hz': centroid(b), 'centroid_game_hz': centroid(a),
        'pitch_reference_hz': reference_pitch, 'pitch_game_hz': game_pitch,
        'active_reference_seconds': active_seconds(b), 'active_game_seconds': active_seconds(a),
        'left_right_correlation': channels, 'mono_as_stereo_blocks': hits, 'blocks': blocks,
    }
    if arguments.json:
        print(json.dumps(result, indent=1))
        return 0
    print('reference   %.3f s, %d channel(s)    game %.3f s%s' % (result['reference_seconds'], reference.shape[1], result['game_seconds'],
          ' (window %.2f-%.2f s)' % (first / RATE, last / RATE) if first or arguments.stop else ''))
    print('lag         %d samples (%.1f ms)' % (lag, result['lag_ms']))
    print('correlation %.4f' % correlation)
    print('gain        %.4f (%.1f dB)' % (gain, 20.0 * np.log10(abs(gain)) if gain else float('-inf')))
    print('snr         %s' % ('exact: every sample equal' if exact else 'none: unrelated' if snr == float('-inf') else '%.1f dB' % snr))
    print('centroid    reference %.0f Hz, game %.0f Hz' % (result['centroid_reference_hz'], result['centroid_game_hz']))
    print('pitch       reference %.0f Hz, game %.0f Hz%s' % (reference_pitch, game_pitch,
          ', ratio %.2f' % (game_pitch / reference_pitch) if reference_pitch and game_pitch else ''))
    print('duration    reference %.2f s, game %.2f s above the noise floor' % (result['active_reference_seconds'], result['active_game_seconds']))
    print('left/right  %.4f' % channels)
    if blocks:
        print('mono-as-stereo  %d of %d blocks' % (hits, blocks))
    return 0


if __name__ == '__main__':
    sys.exit(main())
