#!/usr/bin/env python3
"""Screenshot the panel: ask the board for its framebuffer over USB serial and save a PNG.

  python3 tools/grab.py out.png [commands] [--wait SECONDS]

Commands are sent one per --delay, e.g. "s" (sample data), "w" (next weather scene), "b" (bus
drive-by), "p" (next view); each "d" saves a shot. --wait gives a board that has just restarted time to boot.
"""
import argparse
import glob
import os
import select
import termios
import time
import tty

from PIL import Image

NATIVE_W, TILE_COLS, TILE_ROWS = 300, 38, 50  # ST7305 buffer: 38 x 8 columns, 50 x 8 rows, vertical bytes

ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("commands", nargs="?", default="")
ap.add_argument("--wait", type=float, default=6)
ap.add_argument("--delay", type=float, default=1.0, help="seconds between commands, and before the grab")
ap.add_argument("--port", default=(glob.glob("/dev/cu.usbmodem*") or [""])[0])
a = ap.parse_args()

fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
termios.tcflush(fd, termios.TCIOFLUSH)
time.sleep(a.wait)


def drain(seconds):
    end = time.time() + seconds
    while time.time() < end:
        if select.select([fd], [], [], 0.1)[0]:
            os.read(fd, 65536)


def read_frame():
    """One framebuffer, or None if a log line from the other core landed in the middle of it."""
    drain(0.5)  # finish anything still streaming (an earlier frame, a log line)
    os.write(fd, b"d")
    buf, deadline = b"", time.time() + 10
    while b"\n" not in buf.split(b"FB:", 1)[-1] and time.time() < deadline:
        if select.select([fd], [], [], 0.5)[0]:
            buf += os.read(fd, 65536)
    if b"FB:" not in buf:
        return None
    hexdata = buf.split(b"FB:", 1)[1].split(b"\n", 1)[0].strip()
    try:
        raw = bytes.fromhex(hexdata.decode())
    except ValueError:
        return None
    return raw if len(raw) == TILE_COLS * 8 * TILE_ROWS else None


def grab(out):
    for _ in range(4):
        raw = read_frame()
        if raw:
            break
    else:
        raise SystemExit("no clean frame from the board after 4 tries")
    native = Image.new("1", (TILE_COLS * 8, TILE_ROWS * 8), 1)
    px = native.load()
    for row in range(TILE_ROWS):
        for x in range(TILE_COLS * 8):
            byte = raw[row * TILE_COLS * 8 + x]
            for bit in range(8):
                if byte >> bit & 1:
                    px[x, row * 8 + bit] = 0
    # U8G2_R1 draws landscape onto the portrait panel: undo the quarter turn.
    native.crop((0, 0, NATIVE_W, TILE_ROWS * 8)).rotate(90, expand=True).save(out)
    print("saved", out)


# Each "d" in the commands takes a shot (out.png, out-1.png, ...); with none, one shot at the end.
cmds = a.commands if "d" in a.commands else a.commands + "d"
shots = 0
for c in cmds:
    if c == "d":
        grab(a.out if not shots else a.out.replace(".png", f"-{shots}.png"))
        shots += 1
    else:
        os.write(fd, c.encode())
    time.sleep(a.delay)
os.close(fd)

