#!/usr/bin/env python3
"""Copy JPEGs onto the board's memory card over USB, into /photos, without taking the card out.

  python3 tools/upload.py photo1.jpg photo2.jpg ...

HEIC photos from an iPhone: convert first, e.g. `sips -s format jpeg -Z 1600 IMG.HEIC --out IMG.jpg`.
"""
import glob
import os
import select
import sys
import time
import tty

CHUNK = 256  # must match the firmware's chunk buffer


def wait_for(fd, token, timeout):
    buf, end = b"", time.time() + timeout
    while token not in buf and time.time() < end:
        if select.select([fd], [], [], 0.2)[0]:
            buf += os.read(fd, 4096)
    return buf


fd = os.open(glob.glob("/dev/cu.usbmodem*")[0], os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
for _ in range(30):  # opening the port can restart the board: wait until it answers
    os.write(fd, b"i")
    if b"indoor raw" in wait_for(fd, b"indoor raw", 2):
        break
else:
    sys.exit("the board isn't answering")
for path in sys.argv[1:]:
    data = open(path, "rb").read()
    os.write(fd, b"P" + os.path.basename(path).encode() + b"\n" + str(len(data)).encode() + b"\n")
    reply = wait_for(fd, b"ready", 5)
    if b"ready" not in reply:
        sys.exit(f"{path}: refused ({reply.decode(errors='replace').strip()[-60:]})")
    for i in range(0, len(data), CHUNK):
        os.write(fd, data[i:i + CHUNK])
        if b"." not in wait_for(fd, b".", 5):
            sys.exit(f"{path}: the board stopped acknowledging at byte {i}")
    reply = wait_for(fd, b"\n", 5).decode(errors="replace")
    print(path, "->", reply.strip(". \r\n") or "no reply")
os.close(fd)
