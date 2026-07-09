#!/usr/bin/env python3
"""Send test OSC to LifeRealtime (default port 9000).

Simulates a 4/4 techno pattern: /kick /snare /hihat /beat plus a synthetic
/fft frame at ~30 Hz. Pure stdlib — no python-osc dependency.

Usage:
    python3 tools/send_test_osc.py [--port 9000] [--bpm 128] [--duration 10]
"""

import argparse
import math
import socket
import struct
import time


def osc_pad(b: bytes) -> bytes:
    return b + b"\x00" * ((4 - len(b) % 4) % 4)


def osc_message(address: str, *args: float) -> bytes:
    msg = osc_pad(address.encode() + b"\x00")
    tags = "," + "f" * len(args)
    msg += osc_pad(tags.encode() + b"\x00")
    for a in args:
        msg += struct.pack(">f", a)  # OSC floats are big-endian
    return msg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--bpm", type=float, default=128.0)
    ap.add_argument("--duration", type=float, default=10.0)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dest = (args.host, args.port)

    beat_interval = 60.0 / args.bpm
    tick = 1.0 / 30.0  # feature frame rate
    start = time.time()
    sent = 0

    print(f"sending to {dest} at {args.bpm} BPM for {args.duration}s")
    while True:
        now = time.time() - start
        if now >= args.duration:
            break

        beat_phase = (now / beat_interval) % 4.0
        in_beat = (now % beat_interval) < tick

        if in_beat:
            step = int(beat_phase)
            sock.sendto(osc_message("/kick", 1.0), dest)
            sock.sendto(osc_message("/beat", 1.0), dest)
            if step in (1, 3):
                sock.sendto(osc_message("/snare", 0.9), dest)
            sent += 2

        # 8th-note hihats
        if (now % (beat_interval / 2)) < tick:
            sock.sendto(osc_message("/hihat", 0.7), dest)
            sent += 1

        # Synthetic FFT: bass hump moving with the beat + noise floor.
        env = max(0.0, 1.0 - (now % beat_interval) / beat_interval)
        fft = []
        for i in range(128):
            bass = env * math.exp(-i / 6.0)
            mid = 0.3 * math.exp(-((i - 40) ** 2) / 200.0) * (0.5 + 0.5 * math.sin(now * 2.0))
            noise = 0.05 * ((i * 2654435761 + int(now * 1000)) % 100) / 100.0
            fft.append(min(1.0, bass + mid + noise))
        sock.sendto(osc_message("/fft", *fft), dest)
        sent += 1

        time.sleep(tick)

    print(f"sent {sent} messages")


if __name__ == "__main__":
    main()
