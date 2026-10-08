#!/usr/bin/env python3
"""Draw psp5's own menu sound sets into assets/sfx/<set>/.

    python3 tools/make-sounds.py [--out assets/sfx]

The UI kit ships two recorded sets, glass and paper, and its SoundSet enum has
room for exactly those two. psp5 wanted more to choose from, so it loads sets
itself: a set is a directory of "<cue>_NN.wav", and anything found beside the
kit's two is offered in the settings panel. These are the ones psp5 brings.

Everything here is synthesised - no samples, nothing anyone has to license - and
written as 48 kHz 16-bit stereo, which is what the kit's decoder accepts.

Copyright (C) 2026 the psp5 authors
SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import math
import pathlib
import struct

RATE = 48000


def envelope(n, attack, decay, hold=0.0):
    """Attack, a flat hold, then an exponential tail. Returns a list of gains."""
    out = []
    a = max(1, int(attack * RATE))
    h = int(hold * RATE)
    for i in range(n):
        if i < a:
            g = i / a
        elif i < a + h:
            g = 1.0
        else:
            t = (i - a - h) / RATE
            g = math.exp(-t / max(1e-4, decay))
        out.append(g)
    return out


def sine(freq, n, phase=0.0):
    return [math.sin(2.0 * math.pi * freq * i / RATE + phase) for i in range(n)]


def glide(f0, f1, n):
    """A tone that slides from f0 to f1 - the pitch envelope these cues live on."""
    out = []
    phase = 0.0
    for i in range(n):
        t = i / max(1, n - 1)
        f = f0 * (f1 / f0) ** t
        phase += 2.0 * math.pi * f / RATE
        out.append(math.sin(phase))
    return out


def square(freq, n, duty=0.5):
    out = []
    for i in range(n):
        t = (freq * i / RATE) % 1.0
        out.append(1.0 if t < duty else -1.0)
    return out


def fm(carrier, ratio, index, n):
    """Two operators: the bell-like timbre, without a wavetable."""
    out = []
    for i in range(n):
        t = i / RATE
        m = math.sin(2.0 * math.pi * carrier * ratio * t)
        out.append(math.sin(2.0 * math.pi * carrier * t + index * m))
    return out


def lowpass(samples, cutoff):
    """One pole, to take the edge off a square wave."""
    a = math.exp(-2.0 * math.pi * cutoff / RATE)
    out = []
    y = 0.0
    for x in samples:
        y = (1.0 - a) * x + a * y
        out.append(y)
    return out


def mix(*layers):
    n = max(len(layer) for layer in layers)
    out = [0.0] * n
    for layer in layers:
        for i, v in enumerate(layer):
            out[i] += v
    return out


def shape(samples, env, gain=1.0):
    return [s * e * gain for s, e in zip(samples, env)]


def seconds(value):
    return int(value * RATE)


def write_wav(path, samples, peak=0.5):
    """Normalised to `peak`, so one cue is not twice the loudness of the next."""
    high = max((abs(s) for s in samples), default=0.0)
    scale = (peak / high) if high > 0.0 else 0.0
    # A short fade at both ends: a waveform cut mid-cycle clicks.
    fade = min(240, len(samples) // 4)
    frames = bytearray()
    for i, s in enumerate(samples):
        v = s * scale
        if i < fade:
            v *= i / fade
        elif i >= len(samples) - fade:
            v *= (len(samples) - 1 - i) / fade
        value = max(-32767, min(32767, int(v * 32767)))
        frames += struct.pack('<hh', value, value)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('wb') as fh:
        fh.write(b'RIFF')
        fh.write(struct.pack('<I', 36 + len(frames)))
        fh.write(b'WAVEfmt ')
        fh.write(struct.pack('<IHHIIHH', 16, 1, 2, RATE, RATE * 4, 4, 16))
        fh.write(b'data')
        fh.write(struct.pack('<I', len(frames)))
        fh.write(frames)


# ---- "soft": rounded sine tones, nothing percussive -----------------------

def soft(cue):
    if cue == 'focus':
        n = seconds(0.055)
        return shape(sine(1180, n), envelope(n, 0.004, 0.018))
    if cue == 'select':
        n = seconds(0.16)
        return mix(shape(sine(680, n), envelope(n, 0.004, 0.05)),
                   shape(sine(1020, n), envelope(n, 0.012, 0.045), 0.6))
    if cue == 'back':
        n = seconds(0.16)
        return shape(glide(760, 470, n), envelope(n, 0.005, 0.05))
    if cue == 'tab':
        n = seconds(0.09)
        return shape(sine(880, n), envelope(n, 0.004, 0.03))
    if cue == 'open':
        n = seconds(0.22)
        return shape(glide(520, 880, n), envelope(n, 0.012, 0.07))
    if cue == 'modal_open':
        n = seconds(0.3)
        return mix(shape(sine(523, n), envelope(n, 0.016, 0.1)),
                   shape(sine(784, n), envelope(n, 0.03, 0.09), 0.7))
    if cue == 'modal_close':
        n = seconds(0.24)
        return mix(shape(sine(784, n), envelope(n, 0.008, 0.07), 0.7),
                   shape(sine(523, n), envelope(n, 0.02, 0.08)))
    if cue == 'toggle':
        n = seconds(0.1)
        return shape(sine(940, n), envelope(n, 0.003, 0.028))
    if cue == 'slider':
        n = seconds(0.05)
        return shape(sine(1320, n), envelope(n, 0.002, 0.014))
    if cue == 'error':
        n = seconds(0.26)
        return mix(shape(sine(220, n), envelope(n, 0.006, 0.09)),
                   shape(sine(233, n), envelope(n, 0.006, 0.09), 0.8))
    if cue == 'notify':
        n = seconds(0.26)
        return mix(shape(sine(880, n), envelope(n, 0.006, 0.08)),
                   shape(sine(1320, n), envelope(n, 0.06, 0.07), 0.55))
    if cue == 'launch':
        n = seconds(0.46)
        return mix(shape(glide(392, 784, n), envelope(n, 0.02, 0.16)),
                   shape(sine(1176, n), envelope(n, 0.14, 0.12), 0.4))
    if cue == 'favorite_on':
        n = seconds(0.3)
        return mix(shape(glide(880, 1320, n), envelope(n, 0.008, 0.09)),
                   shape(sine(1760, n), envelope(n, 0.09, 0.07), 0.35))
    if cue == 'favorite_off':
        n = seconds(0.22)
        return shape(glide(1180, 740, n), envelope(n, 0.006, 0.07))
    if cue == 'saved':
        n = seconds(0.32)
        return mix(shape(sine(659, n), envelope(n, 0.01, 0.1)),
                   shape(sine(988, n), envelope(n, 0.07, 0.09), 0.6))
    if cue == 'welcome':
        n = seconds(0.7)
        return mix(shape(sine(392, n), envelope(n, 0.05, 0.26)),
                   shape(sine(523, n), envelope(n, 0.09, 0.24), 0.8),
                   shape(sine(659, n), envelope(n, 0.14, 0.22), 0.6))
    if cue == 'resume':
        n = seconds(0.26)
        return shape(glide(587, 880, n), envelope(n, 0.012, 0.08))
    return None


# ---- "arcade": filtered square blips, closer to a handheld -----------------

def arcade(cue):
    if cue == 'focus':
        n = seconds(0.04)
        return shape(lowpass(square(1046, n, 0.5), 5200), envelope(n, 0.001, 0.012))
    if cue == 'select':
        n = seconds(0.12)
        return shape(lowpass(glide(784, 1568, n), 6000), envelope(n, 0.002, 0.04))
    if cue == 'back':
        n = seconds(0.12)
        return shape(lowpass(glide(880, 440, n), 5000), envelope(n, 0.002, 0.04))
    if cue == 'tab':
        n = seconds(0.07)
        return shape(lowpass(square(698, n, 0.25), 5200), envelope(n, 0.002, 0.022))
    if cue == 'open':
        n = seconds(0.18)
        return shape(lowpass(glide(523, 1046, n), 6000), envelope(n, 0.004, 0.06))
    if cue == 'modal_open':
        n = seconds(0.2)
        return shape(lowpass(square(880, n, 0.3), 5200), envelope(n, 0.004, 0.06))
    if cue == 'modal_close':
        n = seconds(0.18)
        return shape(lowpass(glide(880, 587, n), 5000), envelope(n, 0.003, 0.055))
    if cue == 'toggle':
        n = seconds(0.07)
        return shape(lowpass(square(1318, n, 0.4), 6000), envelope(n, 0.001, 0.02))
    if cue == 'slider':
        n = seconds(0.035)
        return shape(lowpass(square(1568, n, 0.5), 7000), envelope(n, 0.001, 0.01))
    if cue == 'error':
        n = seconds(0.22)
        return shape(lowpass(square(147, n, 0.5), 2400), envelope(n, 0.003, 0.08))
    if cue == 'notify':
        n = seconds(0.24)
        half = n // 2
        return shape(lowpass(square(1046, half, 0.4) + square(1568, n - half, 0.4), 6000),
                     envelope(n, 0.003, 0.09))
    if cue == 'launch':
        n = seconds(0.42)
        third = n // 3
        tones = (square(523, third, 0.35) + square(784, third, 0.35) +
                 square(1046, n - 2 * third, 0.35))
        return shape(lowpass(tones, 6500), envelope(n, 0.004, 0.16))
    if cue == 'favorite_on':
        n = seconds(0.24)
        return shape(lowpass(glide(1046, 2093, n), 7000), envelope(n, 0.003, 0.08))
    if cue == 'favorite_off':
        n = seconds(0.18)
        return shape(lowpass(glide(1568, 784, n), 5500), envelope(n, 0.003, 0.06))
    if cue == 'saved':
        n = seconds(0.26)
        half = n // 2
        return shape(lowpass(square(784, half, 0.3) + square(1175, n - half, 0.3), 6000),
                     envelope(n, 0.004, 0.09))
    if cue == 'welcome':
        n = seconds(0.6)
        quarter = n // 4
        tones = (square(392, quarter, 0.3) + square(523, quarter, 0.3) +
                 square(659, quarter, 0.3) + square(784, n - 3 * quarter, 0.3))
        return shape(lowpass(tones, 6000), envelope(n, 0.01, 0.22))
    if cue == 'resume':
        n = seconds(0.2)
        return shape(lowpass(glide(659, 988, n), 6000), envelope(n, 0.004, 0.07))
    return None


# ---- "bell": FM, long tails, the quietest of the three --------------------

def bell(cue):
    tails = {
        'focus': (0.06, 1480, 1.4, 1.2), 'select': (0.3, 880, 2.0, 2.4),
        'back': (0.26, 660, 2.0, 2.0), 'tab': (0.14, 990, 1.4, 1.6),
        'open': (0.34, 740, 3.0, 2.2), 'modal_open': (0.4, 587, 2.0, 2.6),
        'modal_close': (0.3, 494, 2.0, 2.2), 'toggle': (0.16, 1175, 1.4, 1.4),
        'slider': (0.07, 1760, 1.4, 1.0), 'error': (0.34, 196, 1.41, 3.0),
        'notify': (0.4, 1046, 2.0, 2.0), 'launch': (0.66, 523, 3.0, 3.0),
        'favorite_on': (0.4, 1318, 2.0, 1.8), 'favorite_off': (0.3, 784, 2.0, 1.8),
        'saved': (0.42, 880, 2.0, 2.0), 'welcome': (0.9, 392, 2.0, 3.2),
        'resume': (0.32, 698, 2.0, 2.0),
    }
    if cue not in tails:
        return None
    length, carrier, ratio, index = tails[cue]
    n = seconds(length)
    return shape(fm(carrier, ratio, index, n), envelope(n, 0.004, length * 0.42))


CUES = ['focus', 'select', 'back', 'tab', 'open', 'modal_open', 'modal_close', 'toggle',
        'slider', 'error', 'notify', 'launch', 'favorite_on', 'favorite_off', 'saved',
        'welcome', 'resume']

SETS = {'soft': soft, 'arcade': arcade, 'bell': bell}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', default='assets/sfx', type=pathlib.Path)
    args = parser.parse_args()

    for name, maker in SETS.items():
        written = 0
        for cue in CUES:
            samples = maker(cue)
            if not samples:
                continue
            write_wav(args.out / name / ('%s_01.wav' % cue), samples)
            written += 1
        print('%s: %d cue(s) -> %s' % (name, written, args.out / name))


if __name__ == '__main__':
    main()
