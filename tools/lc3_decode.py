#!/usr/bin/env python3
"""Decode a raw LC3 capture from `g1flash.py mic record` to WAV via liblc3 (brew install liblc3).

The stock G1 stream is 16 kHz mono; each F1 notification carries 200 bytes
that `--frame-bytes` splits into fixed-size frames (default 40 bytes = 10 ms
at 32 kbit/s). Pass --selftest to encode a tone with the same library and
check the round trip without any glasses."""
import argparse
import ctypes
import ctypes.util
import math
import struct
import sys
import wave
from pathlib import Path

LC3_PCM_FORMAT_S16 = 0


def load_lib():
    for name in ('/opt/homebrew/opt/liblc3/lib/liblc3.dylib', ctypes.util.find_library('lc3')):
        if name:
            try:
                lib = ctypes.CDLL(name)
            except OSError:
                continue
            lib.lc3_decoder_size.restype = ctypes.c_uint
            lib.lc3_decoder_size.argtypes = [ctypes.c_int, ctypes.c_int]
            lib.lc3_setup_decoder.restype = ctypes.c_void_p
            lib.lc3_setup_decoder.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
            lib.lc3_decode.restype = ctypes.c_int
            lib.lc3_decode.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
            lib.lc3_frame_samples.restype = ctypes.c_int
            lib.lc3_frame_samples.argtypes = [ctypes.c_int, ctypes.c_int]
            lib.lc3_encoder_size.restype = ctypes.c_uint
            lib.lc3_encoder_size.argtypes = [ctypes.c_int, ctypes.c_int]
            lib.lc3_setup_encoder.restype = ctypes.c_void_p
            lib.lc3_setup_encoder.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
            lib.lc3_encode.restype = ctypes.c_int
            lib.lc3_encode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
            return lib
    raise SystemExit('liblc3 not found; brew install liblc3')


def decode(lib, data, frame_bytes, frame_us, rate):
    mem = ctypes.create_string_buffer(lib.lc3_decoder_size(frame_us, rate))
    dec = lib.lc3_setup_decoder(frame_us, rate, rate, mem)
    samples = lib.lc3_frame_samples(frame_us, rate)
    pcm = (ctypes.c_int16 * samples)()
    out, bad = bytearray(), 0
    for off in range(0, len(data) - frame_bytes + 1, frame_bytes):
        frame = ctypes.create_string_buffer(bytes(data[off:off + frame_bytes]), frame_bytes)
        rc = lib.lc3_decode(dec, frame, frame_bytes, LC3_PCM_FORMAT_S16, pcm, 1)
        if rc != 0:
            bad += 1   # PLC output is still appended
        out += bytes(pcm)
    return bytes(out), bad, samples


def write_wav(path, pcm, rate):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def selftest(lib, frame_bytes, frame_us, rate):
    samples = lib.lc3_frame_samples(frame_us, rate)
    mem = ctypes.create_string_buffer(lib.lc3_encoder_size(frame_us, rate))
    enc = lib.lc3_setup_encoder(frame_us, rate, rate, mem)
    frames = 50
    pcm = (ctypes.c_int16 * (samples * frames))(*[int(8000 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(samples * frames)])
    data = bytearray()
    out = ctypes.create_string_buffer(frame_bytes)
    for f in range(frames):
        block = (ctypes.c_int16 * samples).from_buffer(pcm, f * samples * 2)
        if lib.lc3_encode(enc, LC3_PCM_FORMAT_S16, block, 1, frame_bytes, out) != 0:
            raise SystemExit('encode failed')
        data += out.raw
    decoded, bad, _ = decode(lib, data, frame_bytes, frame_us, rate)
    tail = struct.unpack(f'<{samples * 10}h', decoded[-samples * 20:])
    peak = max(abs(v) for v in tail)
    if bad or not 6000 < peak < 10000:
        raise SystemExit(f'selftest failed: bad={bad} peak={peak}')
    print(f'selftest ok: {frames} frames, {len(data)} bytes, decoded peak {peak}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', nargs='?', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--frame-bytes', type=int, default=40)
    parser.add_argument('--frame-us', type=int, default=10000)
    parser.add_argument('--rate', type=int, default=16000)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    lib = load_lib()
    if args.selftest:
        selftest(lib, args.frame_bytes, args.frame_us, args.rate)
        return
    if not args.capture:
        parser.error('capture file required')
    data = args.capture.read_bytes()
    pcm, bad, samples = decode(lib, data, args.frame_bytes, args.frame_us, args.rate)
    values = struct.unpack(f'<{len(pcm) // 2}h', pcm)
    peak = max((abs(v) for v in values), default=0)
    rms = math.sqrt(sum(v * v for v in values) / len(values)) if values else 0
    out = args.out or args.capture.with_suffix('.wav')
    write_wav(out, pcm, args.rate)
    print(f'{len(data)} bytes -> {len(data) // args.frame_bytes} frames, {len(values) / args.rate:.2f} s, '
          f'{bad} frames flagged by the decoder, peak {peak}, rms {rms:.0f}, wrote {out}', file=sys.stderr)


if __name__ == '__main__':
    main()
