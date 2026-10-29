#!/usr/bin/env python3
"""Set today's local note over USB. An empty note clears it; text is never echoed."""
import argparse
import glob
import os
import select
import time
import tty


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("text", help="up to 56 printable ASCII characters; empty to clear")
    ap.add_argument("--port")
    a = ap.parse_args()
    text = a.text.strip(" ")
    if len(text) > 56 or any(ord(c) < 32 or ord(c) > 126 for c in text):
        ap.error("use up to 56 printable ASCII characters")
    ports = [a.port] if a.port else glob.glob("/dev/cu.usbmodem*")
    if len(ports) != 1:
        ap.error("specify --port when there is not exactly one USB board")
    fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        tty.setraw(fd)
        # Do not toggle DTR/RTS: that can reset this board.
        # Older firmware reads note letters as commands, so verify capability before sending any text.
        os.write(fd, b"F")
        buf = b""
        until = time.monotonic() + 8
        while time.monotonic() < until:
            if select.select([fd], [], [], 0.1)[0]:
                buf = (buf + os.read(fd, 65536))[-4096:]
                if b"focus: phase " in buf:
                    break
        else:
            raise SystemExit("New Daycard firmware not detected. No note was sent.")
        os.write(fd, ("T" + text + "\n").encode("ascii"))
        buf = b""
        until = time.monotonic() + 8
        while time.monotonic() < until:
            if select.select([fd], [], [], 0.1)[0]:
                buf = (buf + os.read(fd, 65536))[-4096:]
                if b"intention: saved" in buf:
                    print("Daily intention saved on the board until midnight Singapore time.")
                    return
                if b"intention: refused" in buf or b"intention: storage failed" in buf:
                    raise SystemExit("Board refused the note; check its clock and storage.")
        raise SystemExit("No confirmation from the board. The note was not confirmed saved.")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
