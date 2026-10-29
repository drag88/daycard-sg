# daycard verification map

This directory is the maintained source for verifying what a daycard user sees and does. Read this index before
driving the board, then use the matching feature file as the recipe.

## Baseline preconditions

- One Waveshare ESP32-S3-RLCD-4.2 on USB at `/dev/cu.usbmodem*`, flashed with the build under test (SKILL.md, Launch).
- The Doctor passes: `voice on`, a non-zero `wifi` dBm, `memory-v1`, and phrases starting with `CALL_NAME`.
- Wi-Fi and an LTA key already saved on the board for anything that needs live buses or weather.
- Know which config the build used (`config.local.h` if present, else `config.h`) and read its `BUS_STOP`,
  `BUS_STOP_NAME`, `BUS_SERVICES`, `NEA_AREA`, `WORLD_CLOCKS`, `CALL_NAME` and `AP_NAME`: they are the expected values.
- Nothing else holds the serial port.

## Driving conventions

- Start each feature from a fresh boot on the card: `serial_ask.py <letters> --reset --wait 30`. Helpers don't
  restart the board on their own, and a view you opened (the garden especially) stays open.
- Load sample data (`s`) before a layout screenshot, so the frame doesn't depend on live feeds. Live feeds replace
  it at their next refresh, and their first refresh comes within about a minute of boot.
- `p` is the same as tapping KEY; `r` the same as tapping BOOT. Say so when a letter stood in for a button.
- Look at every PNG before using it as proof.

## Proof and skip reporting

- Save artifacts as `verify-out/<run>/<feature>-<what>.png|txt`.
- Pair each action (letters sent) with the resulting screen or reply.
- A feature that writes state (garden, focus) is proven by reading the state back (`G`, `F`), not only by a screen.
- Report a path you couldn't drive (live voice needs a person in the room, a long-press needs a hand on the button)
  as unverified with the reason. Don't count a serial letter as proof of the button or voice path it stands in for.

## Feature entry contract

Each feature file starts with an H1 title and one paragraph describing the user-visible behavior, then exactly four
H2 sections in this order: `Sub-features`, `How to get to it (user POV)`, `Driving it with serial_ask.py and
grab.py`, `Gotchas`. Keep implementation details out; name user paths, letters, expected values and observable proof.

## Features

- [Main card](./card.md): clock, world clocks, weather row, quote panel and the cat.
- [Bus board](./buses.md): the configured stop and services with the next three arrivals.
- [Setup page](./setup.md): the Wi-Fi network and password shown for first-time setup.
- [Voice phrases](./voice.md): the phrases the recogniser holds, built from `CALL_NAME`.
- [Focus and garden](./focus-garden.md): a focus session and the permanent garden counts.
