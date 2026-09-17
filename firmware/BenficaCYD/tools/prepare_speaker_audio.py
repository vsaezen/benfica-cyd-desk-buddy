#!/usr/bin/env python3
"""Optimise the Benfica MP3 files for the CYD's mono analogue amplifier."""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

import av
import numpy as np


SAMPLE_RATE = 32000
BIT_RATE = 96000
TARGET_RMS = 0.20
PEAK_LIMIT = 0.92
HIGH_PASS_HZ = 110.0
FILES = ("goal.mp3", "glorioso.mp3", "papoilas.mp3", "hino.mp3")


def decode_mono(path: Path) -> np.ndarray:
    chunks: list[np.ndarray] = []
    with av.open(str(path)) as source:
        resampler = av.AudioResampler(format="fltp", layout="mono", rate=SAMPLE_RATE)
        for frame in source.decode(audio=0):
            for converted in resampler.resample(frame):
                chunks.append(converted.to_ndarray()[0].astype(np.float32, copy=True))
        for converted in resampler.resample(None):
            chunks.append(converted.to_ndarray()[0].astype(np.float32, copy=True))
    if not chunks:
        raise RuntimeError(f"No se pudo decodificar {path.name}")
    return np.concatenate(chunks)


def optimise(samples: np.ndarray) -> tuple[np.ndarray, float, float]:
    samples = samples - np.mean(samples, dtype=np.float64)

    # A first-order high-pass removes bass the tiny enclosure cannot reproduce.
    # Sending that energy to this speaker only consumes headroom and adds buzz.
    alpha = 1.0 / (1.0 + 2.0 * np.pi * HIGH_PASS_HZ / SAMPLE_RATE)
    filtered = np.empty_like(samples)
    previous_x = np.float32(0.0)
    previous_y = np.float32(0.0)
    for index, current_x in enumerate(samples):
        current_y = alpha * (previous_y + current_x - previous_x)
        filtered[index] = current_y
        previous_x = current_x
        previous_y = current_y

    old_rms = float(np.sqrt(np.mean(np.square(filtered), dtype=np.float64)))
    if old_rms > 1e-6:
        filtered *= min(8.0, TARGET_RMS / old_rms)

    # Smooth limiting avoids the hard digital clipping that sounds like crackle.
    filtered = np.tanh(filtered / PEAK_LIMIT) * PEAK_LIMIT
    peak = float(np.max(np.abs(filtered)))
    if peak > PEAK_LIMIT:
        filtered *= PEAK_LIMIT / peak
    new_rms = float(np.sqrt(np.mean(np.square(filtered), dtype=np.float64)))
    return filtered.astype(np.float32, copy=False), old_rms, new_rms


def encode_mp3(samples: np.ndarray, path: Path) -> None:
    with av.open(str(path), mode="w", format="mp3") as target:
        stream = target.add_stream("libmp3lame", rate=SAMPLE_RATE)
        stream.layout = "mono"
        stream.bit_rate = BIT_RATE
        offset = 0
        frame_size = 1152
        while offset < samples.size:
            block = samples[offset : offset + frame_size]
            frame = av.AudioFrame.from_ndarray(block.reshape(1, -1), format="fltp", layout="mono")
            frame.sample_rate = SAMPLE_RATE
            frame.pts = offset
            for packet in stream.encode(frame):
                target.mux(packet)
            offset += block.size
        for packet in stream.encode(None):
            target.mux(packet)


def main() -> None:
    media_dir = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[1] / "sdcard" / "Benfica"
    originals = media_dir / "originals"
    originals.mkdir(exist_ok=True)

    for name in FILES:
        source = media_dir / name
        backup = originals / name
        if not backup.exists():
            shutil.copy2(source, backup)
        samples = decode_mono(backup)
        processed, old_rms, new_rms = optimise(samples)
        temporary = media_dir / f".{name}.new"
        encode_mp3(processed, temporary)
        temporary.replace(source)
        duration = samples.size / SAMPLE_RATE
        print(
            f"{name}: {duration:.1f}s, mono {SAMPLE_RATE} Hz, "
            f"RMS {old_rms:.3f}->{new_rms:.3f}, {source.stat().st_size} bytes"
        )


if __name__ == "__main__":
    main()
