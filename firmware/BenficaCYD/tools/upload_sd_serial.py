#!/usr/bin/env python3
"""Copy Benfica media to the CYD microSD through its USB serial port."""

import glob
import os
import select
import sys
import termios
import time
from pathlib import Path


PORT_PATTERNS = ("/dev/cu.usbserial-*", "/dev/cu.SLAB_USBtoUART")
MEDIA_DIR = Path(__file__).resolve().parents[1] / "sdcard" / "Benfica"
FILES = ("goal.mp3", "glorioso.mp3", "papoilas.mp3", "hino.mp3", "hino.lrc")


def find_port():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for pattern in PORT_PATTERNS:
        matches = sorted(glob.glob(pattern))
        if matches:
            return matches[0]
    raise RuntimeError("No se encuentra el ESP32 por USB")


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    settings = termios.tcgetattr(fd)
    settings[4] = termios.B115200
    settings[5] = termios.B115200
    settings[0] = 0
    settings[1] = 0
    settings[2] = (
        (settings[2] & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB))
        | termios.CS8
        | termios.CLOCAL
        | termios.CREAD
    )
    settings[3] = 0
    settings[6][termios.VMIN] = 0
    settings[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, settings)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def read_line(fd, timeout, wanted):
    deadline = time.monotonic() + timeout
    pending = bytearray()
    while time.monotonic() < deadline:
        readable, _, _ = select.select([fd], [], [], 0.2)
        if not readable:
            continue
        try:
            chunk = os.read(fd, 4096)
        except BlockingIOError:
            continue
        pending.extend(chunk)
        while b"\n" in pending:
            raw, _, remainder = pending.partition(b"\n")
            pending = bytearray(remainder)
            line = raw.decode("utf-8", "replace").strip()
            if line:
                print(line, flush=True)
            if line.startswith(wanted):
                return line
    raise TimeoutError(f"No se recibió {wanted}")


def write_all(fd, payload):
    view = memoryview(payload)
    while view:
        _, writable, _ = select.select([], [fd], [], 2.0)
        if not writable:
            raise TimeoutError("El puerto USB no acepta datos")
        try:
            sent = os.write(fd, view[:4096])
        except BlockingIOError:
            continue
        view = view[sent:]


def write_media_paced(fd, source):
    while True:
        chunk = source.read(1024)
        if not chunk:
            return
        write_all(fd, chunk)
        # Wait until the USB-UART adapter has physically transmitted this
        # block. This prevents overrunning the ESP32 while the SD card flushes.
        termios.tcdrain(fd)
        time.sleep(0.006)


def upload_one(fd, local_path):
    size = local_path.stat().st_size
    remote = f"/Benfica/{local_path.name}"
    command = f"UPLOAD|{remote}|{size}\n".encode("ascii")
    write_all(fd, command)
    read_line(fd, 12, "UPLOAD|READY")

    started = time.monotonic()
    with local_path.open("rb") as source:
        write_media_paced(fd, source)
    read_line(fd, max(20, size / 7000), "UPLOAD|DONE")
    elapsed = max(0.01, time.monotonic() - started)
    print(f"COPIADO {local_path.name}: {size} bytes en {elapsed:.1f}s", flush=True)


def main():
    port = find_port()
    missing = [name for name in FILES if not (MEDIA_DIR / name).is_file()]
    if missing:
        raise RuntimeError("Faltan archivos locales: " + ", ".join(missing))
    fd = open_port(port)
    try:
        time.sleep(0.4)
        write_all(fd, b"TRANSFER|BEGIN\n")
        read_line(fd, 12, "TRANSFER|READY")
        for name in FILES:
            upload_one(fd, MEDIA_DIR / name)
        write_all(fd, b"TRANSFER|END\n")
        read_line(fd, 12, "TRANSFER|DONE")
    finally:
        os.close(fd)
    print("TODOS LOS AUDIOS ESTAN EN LA MICROSD", flush=True)


if __name__ == "__main__":
    main()
