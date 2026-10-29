---
name: verify-daycard
description: Flash a daycard build onto the Waveshare ESP32-S3-RLCD-4.2 and prove what it shows and does, over USB serial and real framebuffer screenshots. Use after any firmware or config.h change, before calling a daycard change done, or when asked to verify the card, bus board, setup page, voice phrases or focus garden on the board.
---

# Verify daycard on the board

The app is firmware on one physical board. You drive it through USB serial: single-letter commands, and
`tools/grab.py`, which pulls the real framebuffer as a PNG. Run every command from the repo root. The feature map in
[features/README.md](features/README.md) lists what to prove and how; read it before driving.

There is one board and one serial port. Never drive it from two shells or agents at once, and close any Arduino
serial monitor first: a second reader steals replies. Opening the port does not restart the board, so state from
an earlier run (sample data, an open view) carries over. `serial_ask.py --reset` gives a fresh boot.

## Launch

Build and flash from the repo root. `sketch.yaml` carries the board options; never pass `-b`.

```sh
PORT=$(ls /dev/cu.usbmodem* | head -1)
arduino-cli compile --build-property upload.maximum_size=6291456 .
arduino-cli upload -p "$PORT" .
```

The build uses `config.local.h` when it exists, otherwise `config.h`. Say which one you flashed. The upload has
landed when each of the five regions ends in `Hash of data verified.`, or, for a region that didn't change,
`No changed sectors found` followed by `'<file>' at <address> verified.` (esptool 5 skips unchanged sectors). Check
the app at `0x00010000` and the speech models at `0x00c10000` in particular.
Ready means usable: about 30 s after the upload the speech models are loaded and Wi-Fi has joined. Check with the
doctor, not a timer. Teardown: nothing keeps running on the computer; the board runs on.

## Doctor

```sh
python3 .claude/skills/verify-daycard/scripts/serial_ask.py iGC --wait 30   # right after Launch
python3 .claude/skills/verify-daycard/scripts/serial_ask.py iGC              # board already up
```

The instance is worth driving when all of these hold:

- `--- i` shows `voice on` and `wifi -NN dBm` (a non-zero number). `wifi 0 dBm` means offline: buses and weather
  won't load, so fix Wi-Fi before proving anything that needs data.
- `--- G` ends in `memory-v1` (the current firmware generation).
- `--- C` lists the phrases, each starting with the configured `CALL_NAME`, ending with `cmd end`.

No reply at all: the port is held by something else, or the board is still booting; rerun with `--wait 40`.

## Drive

Two helpers, both run from the repo root:

- `python3 .claude/skills/verify-daycard/scripts/serial_ask.py <letters> [--reset] [--wait 2] [--listen 3]` sends
  letters and prints each reply. `--reset` restarts the board first (pair it with `--wait 30`); letters that only
  act (`s`, `p`, `r`, `R`, `g`, `k`) print nothing.
- `python3 tools/grab.py <out.png> [letters] --wait 1 [--delay 1]` sends letters one per `--delay` seconds, and each
  `d` saves a screenshot (`out.png`, `out-1.png`, ...). Without a `d` it takes one shot at the end.

Letters that matter for verification: `s` loads sample data so a screenshot doesn't depend on live feeds (live
feeds overwrite it at their next refresh, and a restart clears it), `p` steps
to the next view (card, buses, room, forecast, photos, focus, garden), `g` opens the garden, `k` opens the setup
page, `r` / `R` start and stop focus, `F` / `G` report focus and garden state, `C` lists voice phrases, `i` status,
`f` feed ages. Full list: `handleSerial()` in `daycard.ino` and the daycard skill.

Avoid `H` (hangs the loop on purpose), `W` (drops Wi-Fi), `D<name>` (deletes a photo), `K` (replaces the LTA key)
and `T` (replaces the user's daily intention) unless the run is about exactly that.

## Evidence

Put every artifact under `verify-out/<YYYYMMDD-HHMM>/` (gitignored), named `<feature>-<what>.png` or `.txt`.
Proof standards:

- Drive the real user path where one exists (a view reached with `p`, the same as tapping KEY); say when a serial
  letter stood in for a button or a voice command, since a person has to try the button or voice for that path.
- Capture the action and the resulting state: the serial transcript of what you sent, then the screenshot or reply
  showing the change.
- Check what a feature writes, not only what it draws: `G` counts for the garden, `F` for focus.
- Sample data (`s`) proves layout and config, not live feeds. Prove live data with `f` (feed ages under a few
  minutes) or a screenshot taken without `s`.
- Look at every screenshot before calling it proof. A blank or half-drawn frame is a failed grab; retake it.
- Don't publish `--boot` output, upload logs (they print the board's MAC address) or anything showing the Wi-Fi
  name, the LTA key or the user's intention text.

## Cleanup

Nothing long-running is left on the computer: both helpers close the port when they finish. If the run flashed a
build that isn't the owner's (for example `config.h` examples on a board that has a `config.local.h`), flash their
build back with Launch and rerun the Doctor. `serial_ask.py i --reset --wait 30` clears sample data and leaves the
board on the card. Evidence stays in `verify-out/`; cleanup never deletes it.

## Helpers

- `scripts/serial_ask.py`: send letters and print replies; `--reset` for a fresh boot.
- `tools/grab.py`: framebuffer to PNG (in the repo, shared with development).
