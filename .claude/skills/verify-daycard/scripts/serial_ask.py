#!/usr/bin/env python3
"""Send daycard serial commands and print what the board says back.

  serial_ask.py COMMANDS [--reset] [--wait 2] [--listen 3] [--port /dev/cu.usbmodem...] [--boot]

Opening the port does not restart the board. --reset restarts it first (esptool's hard-reset pulse on RTS), which
also clears sample data; give it --wait 30 for the speech models and Wi-Fi. It waits --wait seconds, then each
command letter gets --listen seconds of replies, printed under "--- <letter>". --boot also prints what the board
logged during --wait, which can name the Wi-Fi network: keep it out of anything you publish. Letters that only act
(s, p, r, R, g, k) print nothing.
"""
import argparse
import fcntl
import glob
import os
import struct
import select
import sys
import termios
import time
import tty

ap = argparse.ArgumentParser()
ap.add_argument("commands")
ap.add_argument("--reset", action="store_true")
ap.add_argument("--wait", type=float, default=2)
ap.add_argument("--listen", type=float, default=3)
ap.add_argument("--port", default=(glob.glob("/dev/cu.usbmodem*") or [""])[0])
ap.add_argument("--boot", action="store_true")
a = ap.parse_args()
if not a.port:
    sys.exit("no /dev/cu.usbmodem* port: is the board plugged in, and is nothing else holding the port?")

fd = os.open(a.port, os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
if a.reset:  # RTS high with DTR low holds the chip in reset, as esptool's hard reset does
    fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_DTR))
    fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_RTS))
    time.sleep(0.2)
    fcntl.ioctl(fd, termios.TIOCMBIC, struct.pack("I", termios.TIOCM_RTS))
termios.tcflush(fd, termios.TCIOFLUSH)


def read_for(seconds):
    out, end = b"", time.time() + seconds
    while time.time() < end:
        if select.select([fd], [], [], 0.1)[0]:
            out += os.read(fd, 65536)
    return out.decode("utf-8", "replace")


boot = read_for(a.wait)
if a.boot:
    print(f"--- boot\n{boot.rstrip()}")
for c in a.commands:
    os.write(fd, c.encode())
    reply = read_for(a.listen)
    print(f"--- {c}\n{reply.rstrip()}")
os.close(fd)
