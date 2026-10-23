#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
Channel-ID test signal for the snd-usb-motu loopback tests.

Every playback channel carries its own sine tone. Recorded through a
loopback (device-internal routing or cables), the dominant frequency of
each capture channel tells which playback channel it receives, so a
shifted channel mapping after a stream gap is detected per time window.

  stream    write raw S24_3LE frames to stdout (pipe into aplay -t raw)
  analyze   check a raw S24_3LE recording (from arecord -t raw)
  selftest  synthesize a recording with a mid-stream shift and analyze it
"""
import argparse
import json
import os
import sys
import tempfile

import numpy as np

BASE_HZ = 250
STEP_HZ = 110
LEVEL = 0.25                    # -12 dBFS per tone
SILENCE = 10 ** (-50 / 20)      # RMS below -50 dBFS counts as silent
FULL_SCALE = 2 ** 23


def tone_freqs(channels):
    return [BASE_HZ + STEP_HZ * c for c in range(channels)]


def to_s24le(samples):
    """int32 array (frames, channels) -> interleaved S24_3LE bytes"""
    b = samples.astype('<i4').view(np.uint8).reshape(-1, 4)[:, :3]
    return b.tobytes()


def tone_block(rate, channels, seconds=1, roll=0):
    """One block of tones; integer Hz tones loop seamlessly every second."""
    t = np.arange(rate * seconds) / rate
    block = np.empty((len(t), channels), dtype=np.int32)
    for c, f in enumerate(tone_freqs(channels)):
        block[:, c] = np.round(LEVEL * (FULL_SCALE - 1) *
                               np.sin(2 * np.pi * f * t))
    if roll:
        block = np.roll(block, roll, axis=1)
    return block


def cmd_stream(args):
    data = to_s24le(tone_block(args.rate, args.channels))
    out = sys.stdout.buffer
    try:
        for _ in range(args.seconds):
            out.write(data)
        out.flush()
    except BrokenPipeError:
        pass
    return 0


def parse_map(spec, channels):
    """'identity' or 'cap:pb,cap:pb' (1-based) -> [(cap, pb)] 0-based"""
    if spec == 'identity':
        return [(c, c) for c in range(channels)]
    pairs = []
    for item in spec.split(','):
        cap, pb = item.split(':')
        pairs.append((int(cap) - 1, int(pb) - 1))
    return pairs


def read_windows(path, channels, rate, window):
    raw = np.memmap(path, dtype=np.uint8, mode='r')
    frame_bytes = 3 * channels
    frames = len(raw) // frame_bytes
    n = int(rate * window)
    for start in range(0, frames - n + 1, n):
        chunk = np.asarray(raw[start * frame_bytes:(start + n) * frame_bytes])
        chunk = chunk.reshape(n, channels, 3).astype(np.int32)
        val = chunk[..., 0] | (chunk[..., 1] << 8) | (chunk[..., 2] << 16)
        val = (val ^ 0x800000) - 0x800000
        yield start / rate, val.astype(np.float64) / FULL_SCALE


def detect(x, rate, freqs):
    """Return the playback channel whose tone dominates x, or None if silent."""
    if np.sqrt(np.mean(x ** 2)) < SILENCE:
        return None
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    peak = np.argmax(spec[1:]) + 1
    f = peak * rate / len(x)
    idx = int(round((f - BASE_HZ) / STEP_HZ))
    if 0 <= idx < len(freqs) and abs(freqs[idx] - f) < STEP_HZ / 4:
        return idx
    return -1                   # signal, but not one of the test tones


def classify(path, channels, rate, window, mapping):
    freqs = tone_freqs(channels)
    windows = []
    for t, data in read_windows(path, channels, rate, window):
        det = {cap: detect(data[:, cap], rate, freqs) for cap, _ in mapping}
        if all(d is None for d in det.values()):
            status, offset = 'silent', None
        elif all(det[cap] == pb for cap, pb in mapping):
            status, offset = 'ok', 0
        else:
            status = 'bad'
            offs = [det[cap] - pb for cap, pb in mapping
                    if det[cap] is not None and det[cap] >= 0]
            offset = max(set(offs), key=offs.count) if offs else None
        windows.append({'t': round(t, 3), 'tr': round(t, 3),
                        'status': status, 'offset': offset})

    # time base: first window with signal (≈ playback start)
    sig = [i for i, w in enumerate(windows) if w['status'] != 'silent']
    if not sig:
        return [], None
    first, last = sig[0], sig[-1]
    t0 = windows[first]['t']
    body = windows[first:last + 1]
    for w in body:
        w['t'] = round(w['t'] - t0, 3)
    return body, t0


def parse_segments(spec):
    """'0-9:ok,12-24:bad,28-:ok' -> [(start, end|None, want)]"""
    segs = []
    for item in spec.split(','):
        rng, want = item.split(':')
        a, b = rng.split('-')
        segs.append((float(a), float(b) if b else None, want))
    return segs


def evaluate(body, expect, segments, window, time_key='t'):
    statuses = [w['status'] for w in body]
    verdict = {'expect': expect, 'ok': True, 'reasons': []}

    if expect == 'clean':
        bad = [w['t'] for w in body if w['status'] != 'ok']
        if bad:
            verdict['ok'] = False
            verdict['reasons'].append('non-ok windows at t=%s' % bad[:10])
    elif expect == 'recovered':
        tail = statuses[-3:]
        if len(tail) < 3 or any(s != 'ok' for s in tail):
            verdict['ok'] = False
            verdict['reasons'].append('last windows not ok: %s' % tail)
    elif expect == 'segments':
        for a, b, want in parse_segments(segments):
            sel = [w for w in body if w[time_key] >= a and
                   (b is None or w[time_key] + window <= b)]
            name = '%g-%s' % (a, 'end' if b is None else '%g' % b)
            if not sel:
                verdict['ok'] = False
                verdict['reasons'].append(
                    'segment %s: no signal (playback stopped?)' % name)
                continue
            n_ok = sum(w['status'] == 'ok' for w in sel)
            n_bad = sum(w['status'] == 'bad' for w in sel)
            if want == 'ok' and n_ok != len(sel):
                verdict['ok'] = False
                verdict['reasons'].append(
                    'segment %s: %d/%d windows ok' % (name, n_ok, len(sel)))
            elif want == 'bad' and n_bad * 2 <= len(sel):
                verdict['ok'] = False
                verdict['reasons'].append(
                    'segment %s: only %d/%d windows shifted' %
                    (name, n_bad, len(sel)))
    return verdict


def cmd_analyze(args):
    mapping = parse_map(args.map, args.channels)
    body, t0 = classify(args.input, args.channels, args.rate, args.window,
                        mapping)
    if not body:
        result = {'signal': False, 'verdict': {'ok': False, 'reasons': [
            'no test tones in the recording: is the loopback set up?']}}
        rc = 2
    else:
        verdict = evaluate(body, args.expect, args.segments, args.window,
                           'tr' if args.time_base == 'record' else 't')
        offsets = sorted({w['offset'] for w in body
                          if w['status'] == 'bad' and w['offset'] is not None})
        result = {
            'signal': True,
            'lead_in_s': t0,
            'windows': len(body),
            'ok': sum(w['status'] == 'ok' for w in body),
            'bad': sum(w['status'] == 'bad' for w in body),
            'silent': sum(w['status'] == 'silent' for w in body),
            'shift_offsets': offsets,
            # one character per window: . ok, X shifted, _ silent
            'timeline': ''.join({'ok': '.', 'bad': 'X', 'silent': '_'}
                                [w['status']] for w in body),
            'verdict': verdict,
            # per window: seconds since recording start, status
            'detail': [[w['tr'], w['status']] for w in body],
            # per window, aligned with detail: channel offset of a shift
            'offsets': [w['offset'] for w in body],
        }
        rc = 0 if verdict['ok'] else 1
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(result, f, indent=1)
    summary = {k: v for k, v in result.items()
               if k not in ('verdict', 'detail', 'offsets')}
    print(json.dumps(summary))
    print('verdict:', 'PASS' if rc == 0 else 'FAIL',
          '; '.join(result['verdict']['reasons']))
    return rc


def cmd_selftest(args):
    rate, ch = 48000, 24
    lead = np.zeros((int(rate * 0.3), ch), dtype=np.int32)
    parts = [lead,
             tone_block(rate, ch, 2),
             tone_block(rate, ch, 2, roll=6),       # channels shifted by 6
             tone_block(rate, ch, 2),
             lead]
    fd, path = tempfile.mkstemp(suffix='.raw')
    try:
        with os.fdopen(fd, 'wb') as f:
            for p in parts:
                f.write(to_s24le(p))
        failures = 0
        for expect, segs, want_rc in (
                ('segments', '0-1.8:ok,2.2-3.8:bad,4.2-:ok', 0),
                ('recovered', '', 0),
                ('clean', '', 1)):
            ns = argparse.Namespace(input=path, channels=ch, rate=rate,
                                    window=0.5, map='identity', expect=expect,
                                    segments=segs, json=None,
                                    time_base='signal')
            rc = cmd_analyze(ns)
            if rc != want_rc:
                print('selftest: expect=%s returned %d, wanted %d' %
                      (expect, rc, want_rc))
                failures += 1
        print('selftest:', 'PASS' if not failures else 'FAIL')
        return 1 if failures else 0
    finally:
        os.unlink(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)

    s = sub.add_parser('stream')
    s.add_argument('--rate', type=int, default=48000)
    s.add_argument('--channels', type=int, default=24)
    s.add_argument('--seconds', type=int, required=True)

    a = sub.add_parser('analyze')
    a.add_argument('input')
    a.add_argument('--rate', type=int, default=48000)
    a.add_argument('--channels', type=int, default=24)
    a.add_argument('--window', type=float, default=0.5)
    a.add_argument('--map', default='identity',
                   help="'identity' or capture:playback pairs, 1-based, e.g. 1:1,2:2")
    a.add_argument('--expect', choices=('clean', 'recovered', 'segments'),
                   default='clean')
    a.add_argument('--segments', default='',
                   help="for --expect segments, e.g. '0-9:ok,12-24:bad,28-:ok'")
    a.add_argument('--time-base', choices=('signal', 'record'),
                   default='signal',
                   help='segment times from the first window with signal '
                        '(default) or from the start of the recording')
    a.add_argument('--json', help='write the full result here')

    sub.add_parser('selftest')

    args = ap.parse_args()
    return {'stream': cmd_stream, 'analyze': cmd_analyze,
            'selftest': cmd_selftest}[args.cmd](args)


if __name__ == '__main__':
    sys.exit(main())
