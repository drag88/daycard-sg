# Contributing

Fixes, new views, voice phrases and better docs are all welcome. Keep each pull request to one change, and use the
commit style already in the history (`feat:`, `fix:`, `docs:`).

## Before you open a pull request

Run the host tests; they need no board:

```sh
c++ -std=c++17 tools/test_logic.cpp -o /tmp/t && /tmp/t
c++ -std=c++17 tools/test_keepsakes.cpp -o /tmp/keepsakes && /tmp/keepsakes
python3 -m unittest discover -s tools -p test_memory.py
```

Build with the example `config.h` (move your `config.local.h` aside first, or build from a fresh clone), so the
change works for someone who hasn't edited anything:

```sh
arduino-cli compile --build-property upload.maximum_size=6291456 .
```

A change to logic in `logic.h` or `keepsakes.h` comes with a test in `tools/`. A change to anything on screen comes
with a screenshot from a real board (`python3 tools/grab.py shot.png --wait 1`) in the pull request.

Keep `config.h` neutral: no real home stop, area or names. Your own values belong in `config.local.h`, which git
ignores.

## Checking a change on the board

`.claude/skills/verify-daycard/SKILL.md` is the full recipe, readable without Claude Code: flash, run the doctor, then
drive each feature in `features/` over USB serial. The short version:

```sh
arduino-cli upload -p /dev/cu.usbmodem* .
python3 .claude/skills/verify-daycard/scripts/serial_ask.py iGC --wait 30   # voice on, Wi-Fi up, phrases loaded
```

## Refreshing the README images

The images in `docs/images/` are real framebuffer captures, tinted to the panel's colours by
`tools/readme_images.py`. Capture them with the example `config.h` on the board, so nobody's own stop or area shows:

```sh
mkdir -p shots && P=.claude/skills/verify-daycard/scripts/serial_ask.py
python3 $P f --reset --wait 75                                              # fresh boot, live feeds loaded
python3 tools/grab.py shots/v.png dpdpdpdpdpd --wait 1 --delay 4             # card, buses, room, forecast, photos, focus
python3 $P f --reset --wait 75
python3 tools/grab.py shots/s.png wdwdwdwdwdwdwdwdwdwdw --wait 1 --delay 2  # the ten weather scenes, sun first
python3 tools/grab.py shots/setup.png k --wait 1 --delay 5
cd shots && mv v.png card.png && mv v-1.png buses.png && mv v-2.png room.png && mv v-3.png forecast.png &&
  mv v-4.png photos.png && mv v-5.png focus.png && mv s.png scene-0.png &&
  for i in 1 2 3 4 5 6 7 8 9; do mv s-$i.png scene-$i.png; done && cd ..
```

The morning strip and the drive-by only appear in the morning routine. Flash a build with
`MORNING_HOUR = 0, WORK_HOUR = 23, EVENING_HOUR = 23`, then:

```sh
python3 tools/grab.py shots/morning.png dbd --wait 1 --delay 1.7 && mv shots/morning-1.png shots/driveby.png
```

Flash your normal build back, then build the images and look at every one before committing. The photo should be a
credited nature photo, not one of your postcards, and nothing should show your own stop, area or intention:

```sh
python3 tools/readme_images.py shots
```

`shots/` is gitignored.
