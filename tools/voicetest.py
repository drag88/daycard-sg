#!/usr/bin/env python3
"""Prompted voice test with timing (python3 tools/voicetest.py [rounds]): each round the board shows 'Recording: speak now' for 8 s and records mic 1.
Reports what Flubber heard and how long after the end of your speech the command arrived."""
import glob, os, select, sys, time, tty
import numpy as np
rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3
fd = os.open(glob.glob("/dev/cu.usbmodem*")[0], os.O_RDWR | os.O_NOCTTY)
tty.setraw(fd)
for r in range(1, rounds + 1):
    t0 = time.time()
    os.write(fd, b"A")
    buf, events, end = b"", [], time.time() + 30
    while time.time() < end:
        if select.select([fd], [], [], 0.05)[0]:
            chunk = os.read(fd, 65536)
            for line in chunk.split(b"\n"):
                if line.startswith(b"voice:"):
                    events.append((time.time() - t0, line.decode(errors="replace").strip()))
            buf += chunk
            if b"PCM:" in buf and b"\n" in buf.split(b"PCM:", 1)[1] and time.time() - t0 > 12:
                break
    try:
        pcm = np.frombuffer(bytes.fromhex(buf.split(b"PCM:", 1)[1].split(b"\n", 1)[0].strip().decode()), "<i2").astype(np.float32)
    except (IndexError, ValueError):
        print(f"round {r}: recording garbled"); continue
    rms = np.array([np.sqrt(np.mean(pcm[i:i + 800] ** 2)) for i in range(0, len(pcm) - 800, 800)])  # 50 ms windows
    loud = np.where(rms > max(150, 5 * np.median(rms)))[0]
    if not len(loud):
        print(f"round {r}: no speech in the recording; Flubber heard {events or 'nothing'}"); continue
    start, stop = loud[0] * 0.05, (loud[-1] + 1) * 0.05
    heard = [f"{e[1][7:]} at +{e[0] - stop:.1f}s after you stopped" for e in events]
    print(f"round {r}: speech {start:.1f}-{stop:.1f}s into the window; Flubber: {heard or 'nothing'}")
    time.sleep(1.5)
os.close(fd)
