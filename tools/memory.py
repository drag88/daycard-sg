#!/usr/bin/env python3
"""Add a dated personal postcard to Daycard over USB, or to a mounted memory card.

  python3 tools/memory.py photo.heic --date 2024-09 --caption "Beach trip"
  python3 tools/memory.py photo.jpg --date 2024-09-24 --caption "A lovely afternoon" --card /Volumes/DAYCARD

Uses the project's existing Pillow dependency; HEIC conversion uses macOS sips.
"""
import argparse
import datetime
import glob
import hashlib
import io
import json
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import tempfile
import time
import tty

CHUNK = 256


def metadata(date, caption):
    if not re.fullmatch(r"\d{4}-\d{2}(?:-\d{2})?", date, flags=re.ASCII):
        raise ValueError("date must be YYYY-MM or YYYY-MM-DD")
    parsed = datetime.date.fromisoformat(date + "-01" if len(date) == 7 else date)
    if parsed.year < 1900:
        raise ValueError("use a date from 1900 onwards")
    caption = caption.strip(" ")
    if not caption or len(caption) > 56 or any(ord(c) < 32 or ord(c) > 126 for c in caption):
        raise ValueError("caption must contain 1 to 56 printable ASCII characters")
    return json.dumps({"date": date, "caption": caption}, separators=(",", ":")).encode("ascii")


def prepare_photo(path):
    from PIL import Image, ImageOps

    with tempfile.TemporaryDirectory(prefix="daycard-photo-") as tmp:
        source = Path(path)
        if source.suffix.lower() in (".heic", ".heif"):
            if not shutil.which("sips"):
                raise ValueError("HEIC conversion needs macOS sips; use a JPEG on this computer")
            converted = Path(tmp) / "converted.jpg"
            subprocess.run(["sips", "-s", "format", "jpeg", str(source), "--out", str(converted)],
                           check=True, capture_output=True)
            source = converted
        with Image.open(source) as image:
            image = ImageOps.exif_transpose(image).convert("RGB")
            image.thumbnail((1200, 1200))
            out = io.BytesIO()
            image.save(out, "JPEG", quality=85, progressive=False)
    data = out.getvalue()
    if len(data) > 2 * 1024 * 1024:
        raise ValueError("prepared photo exceeds the board's 2 MB upload limit")
    return data


class Board:
    def __init__(self, fd):
        self.fd = fd
        self.pending = b""

    def write(self, data):
        until = time.monotonic() + 8
        while data and time.monotonic() < until:
            if select.select([], [self.fd], [], 0.2)[1]:
                data = data[os.write(self.fd, data):]
        if data:
            raise RuntimeError("USB write timed out")

    def reply(self, prefix=b"memory: ", timeout=8):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                line = line.rstrip(b"\r")
                if line.startswith(prefix):
                    return line
            if select.select([self.fd], [], [], 0.2)[0]:
                chunk = os.read(self.fd, 65536)
                if not chunk:
                    raise RuntimeError("USB connection closed")
                self.pending = (self.pending + chunk)[-65536:]
        raise RuntimeError("the board did not confirm the transfer; no further data was sent")

    def check(self):
        # Older firmware treats each text byte as a command. Probe before sending any user content.
        self.write(b"G")
        if b"memory-v1" not in self.reply(b"garden: "):
            raise RuntimeError("install the new Daycard firmware before loading memories")

    def upload(self, name, data):
        self.write(b"U" + name.encode("ascii") + b"\n" + str(len(data)).encode("ascii") + b"\n")
        reply = self.reply()
        if reply == b"memory: exists":
            return
        if reply != b"memory: ready":
            raise RuntimeError("board refused the upload; check the memory card")
        for i in range(0, len(data), CHUNK):
            self.write(data[i:i + CHUNK])
            if self.reply() != b"memory: ack":
                raise RuntimeError("board could not write the postcard")
        if self.reply() != b"memory: saved":
            raise RuntimeError("board could not save the postcard")


def save_to_card(root, name, photo, sidecar):
    dest = Path(root) / "memories"
    dest.mkdir(parents=True, exist_ok=True)
    for filename, data in [(name, photo), (name + ".json", sidecar)]:
        final = dest / filename
        if final.exists():
            if final.read_bytes() != data:
                raise ValueError("an existing postcard has different contents; it was preserved")
            continue
        part = dest / (filename + ".part")
        try:
            with part.open("wb") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            part.replace(final)
        finally:
            part.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("photo", type=Path)
    ap.add_argument("--date", required=True, help="YYYY-MM or YYYY-MM-DD")
    ap.add_argument("--caption", required=True, help="up to 56 printable ASCII characters")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--port", help="USB serial port when multiple boards are attached")
    mode.add_argument("--card", type=Path, help="mounted card root, or a folder to prepare for copying to the card")
    a = ap.parse_args()
    try:
        sidecar = metadata(a.date, a.caption)
        photo = prepare_photo(a.photo)
        digest = hashlib.sha256(photo + sidecar).hexdigest()[:20]
        name = f"memory-{digest}.jpg"
        if a.card:
            save_to_card(a.card, name, photo, sidecar)
            print(f"Postcard ready in {a.card / 'memories'}; keep the JPEG and JSON together.")
            return
        ports = [a.port] if a.port else glob.glob("/dev/cu.usbmodem*")
        if len(ports) != 1:
            ap.error("specify --port when there is not exactly one USB board")
        fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            tty.setraw(fd)  # avoid explicitly resetting the board with DTR/RTS
            board = Board(fd)
            board.check()
            board.upload(name, photo)
            board.upload(name + ".json", sidecar)
            board.write(b"n")
        finally:
            os.close(fd)
        print("Personal postcard saved. It will appear in photo mode.")
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        ap.exit(1, f"Postcard not confirmed saved: {exc}\n")


if __name__ == "__main__":
    main()
