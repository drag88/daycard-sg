---
name: daycard
description: Set up, build, flash, debug or change the daycard firmware on the Waveshare ESP32-S3-RLCD-4.2 desk display (12-hour clock, NEA weather, LTA bus arrivals, room chart, photo frame, focus garden, offline voice commands, self-healing). Use for anything on that board or in this repo - flashing, serial diagnostics, Wi-Fi/bus/weather/voice/temperature problems, configuring config.h, adding views or voice phrases, photos, quotes, calibration, restoring factory firmware.
---

# daycard

The repo README is the user-facing manual
(views, buttons, phrases, self-healing); this skill is the working knowledge behind it. Board facts and pins are in
[references/hardware.md](references/hardware.md). Read the code before changing it; `daycard.ino` is long, grep it.

## Build and flash

```sh
arduino-cli compile --build-property upload.maximum_size=6291456 .
arduino-cli upload -p /dev/cu.usbmodem* .
```

- `sketch.yaml` is the default profile: it pins the FQBN, esp32 core 3.3.12, U8g2 2.36.19, ArduinoJson 7.4.3 and
  WiFiManager 2.0.17, and the first compile downloads them. Don't pass `-b`; the profile carries the board options.
- Every board option matters. `esp_sr_16` makes upload also write the speech models (srmodels.bin) to 0xC10000;
  `partitions.csv` in the sketch folder overrides that scheme's table with one 6 MB app (the build is ~3.7 MB);
  `upload.maximum_size` lifts the 3 MB check; `LoopCore=0` keeps the display off the recogniser's core.
  Delete `partitions.csv` and the Flubber build no longer fits. NVS (Wi-Fi, LTA key) sits at 0x9000 in every
  layout, so reflashing keeps them.
- After a flash, give it ~30 s (speech models load, Wi-Fi joins) before talking to it.
- New machine: install arduino-cli; the profile fetches the rest. On Apple Silicon without Rosetta the bundled
  `ctags` (x86_64) fails every compile with "bad CPU type": install Rosetta
  (`softwareupdate --install-rosetta --agree-to-license`), or build arduino/ctags from source after renaming
  `__unused__` to `CTAGS_UNUSED` in its .c/.h files and drop the binary into
  `<arduino data dir>/packages/builtin/tools/ctags/5.8-arduino11/`.
- Logic tests run on the Mac: `c++ -std=c++17 tools/test_logic.cpp -o /tmp/t && /tmp/t` (DST, bus timing, parsing,
  quote rotation, battery curve, RH correction). Add a test with any change to `logic.h`.

## Talking to the board

Serial over USB (`/dev/cu.usbmodem*`), single-letter commands; open the port raw (`tty.setraw`) from Python, as the
tools do. Logs from the network core can interleave with long replies, so retry on garbage.

| Cmd | Does |
|---|---|
| `i` | status: raw/shown temp+RH, uptime, Wi-Fi dBm, SD, history, voice + listens, frames, sound/floor/quiet |
| `f` | each feed's last success, held outside temp/RH/wind, nowcast/UV/PSI, RTC drift, largest heap block |
| `h` | I2C scan (expect 0x18 ES8311, 0x40 ES7210, 0x51 RTC, 0x70 SHTC3) |
| `m` | mic bytes read (should grow 62.5 KB/s) |
| `C` | phrases the recogniser holds, with phonemes |
| `A` | record 8 s of mic 1 with a "Recording: speak now" prompt, then print `PCM:` hex |
| `d` | framebuffer as `FB:` hex (use `tools/grab.py`) |
| `p` / `n` / `q` / `w` | next view / next photo / fetch a new quote / cycle weather scene (testing) |
| `b` / `r` / `c` | demo bus drive-by / cycle reminder like BOOT / play chime |
| `z` / `W` / `H` | pretend idle / drop Wi-Fi / hang loop (tests the watchdog, restarts in ~2 min) |
| `L` / `D<name>` / `P<name>` | count photos / delete one / upload (use `tools/upload.py`) |
| `K<key>` | set the LTA key (read it from a file; never type or print it) |
| `k` / `s` | open the setup portal / load sample data (hides setup until restart) |

Tools: `grab.py out.png [cmds]` screenshots the real framebuffer; `upload.py *.jpg` copies photos to the card;
`nature.py dir` refetches the 227 curated Unsplash photos as baseline JPEGs; `make_assets.py` re-renders clock
digits, built-in quotes and the quote glyphs into `assets.h` (run it with `DYLD_LIBRARY_PATH=/opt/homebrew/lib` so
Pillow uses raqm, or every existing bitmap changes); `voicetest.py [n]` runs prompted voice rounds and reports what Flubber heard
and how fast.

## Debugging: symptom, then the usual cause

- **Buses or weather frozen / "--"**: `f` for feed ages, `i` for Wi-Fi. `health()` rejoins Wi-Fi after 20 s,
  rebuilds at 20 min without data, restarts at 45 min; `daycard/restarts.log` on the card says what happened.
  data.gov.sg 403s Python's default User-Agent but not the board; test NEA with `curl`.
- **Wi-Fi weak or flapping (NO_AP_FOUND)**: physical first. The usual signal is -40 dBm; it fell to -77 dBm when
  something sat over the module's antenna. Reconnect storms on core 0 starve ESP-SR's audio feed
  ("fetch failed"), so a Wi-Fi problem shows up as Flubber going deaf.
- **Flubber misses or mishears**: `i` shows voice on and listens/min near 10.4 (9.3 measured as fine); `C`
  confirms phrases; `voicetest.py` shows what it heard. The Mac's speakers don't reach the board's mic, so test
  with a person. Avoid adding phrases that share a long opening: six "flubber remind me for ..." phrases pulled
  every unclear command into a false bus reminder, which is why reminders are BOOT-only. To see ESP-SR errors,
  build with `,DebugLevel=warn` (but it garbles `grab.py`).
- **Display stalls**: ESP-SR's detector runs at priority 5 on core 1; with the loop there it got 5 frames in
  2 minutes. Keep `LoopCore=0`. Expect ~24 fps.
- **Indoor temperature reads high**: the SHTC3 sits beside a hot ESP32 (raw ~35 °C with voice on).
  `TEMP_OFFSET_C` (-5.4) and `RH_OFFSET` (-4) in `config.h` came from one board against a room meter. To redo: meter beside the card, read `i` at the same moment, set `TEMP_OFFSET_C` = meter - raw, flash, then set
  `RH_OFFSET` = old `RH_OFFSET` + meter RH - shown RH (shown already includes the old offset). `rhAt` scales RH by about 6 points per degree of correction, so get the
  temperature right first. The room chart recomputes all history. Opening the port with DTR/RTS forced low resets
  the board; pyserial's defaults don't.
- **No photos**: the frame appears after `IDLE_MINUTES` (5) of quiet, or 4 taps of KEY, or "Flubber, show photos".
  The ROM decoder only reads baseline JPEGs: Picsum, Unsplash and iPhone HEIC must be re-encoded first
  (`nature.py` does; `sips -s format jpeg` for HEIC).
- **Quote stuck or built-in**: the serial log prints `quote: <page> -> <HTTP code>, <n> new lines that fit` per
  fetch. `0 new lines` then `reusing the one from N quotes ago` is normal for authors with small pages (14 of the
  46 are under 20 KB); `0 new lines` alone means nothing on the page fits the card (swap the author); a 403 means
  Wikimedia didn't like the agent. Test the parser against live pages on the Mac by compiling `quote.h` with a
  small harness, as `tools/test_logic.cpp` does.
- **Battery**: no charge-detect pin; percent comes from voltage (3:1 divider on GPIO4) and reads high while
  charging. A dead or over-discharged 18650 sits at ~1.8 V; the red WRN LED means it's reversed. Voice is always
  on, so battery life is short; most boards run plugged in.

## Changing things

- Location, buses, world clocks, the voice name (`CALL_NAME`), routine hours, timings, quote authors and
  calibration: `config.h`, or the user's gitignored `config.local.h` copy, which the build prefers when it exists.
- New view: add to `enum View`, write a `draw...` function, add it to the render chain in `loop()`. Views except
  the card and photos return to the card after 60 s.
- New voice phrase: `VOICE_COMMANDS` in `voice.h` (plain English after the `CALL_NAME` prefix), handle it in
  `handleVoice()`, then check `C` and run `voicetest.py`.
- Quotes: live ones come from `QUOTE_AUTHORS` (Wikiquote pages) via `quote.h`, which parses wikitext and sets the
  quote in Newsreader from glyphs baked into `assets.h`; built-ins are `tools/quotes.txt`, then `make_assets.py`.
  The card's box takes two lines, not three (three lines at 16 px need 77 px of a 72 px box). World clocks: `WORLD_CLOCKS` in `config.h`; the DST rules (`DstRule`) are in `logic.h`, with tests.
- Things that bit before: `TX` collides with the core's pin constant; signed overflow in a pixel hash made GCC
  emit an infinite loop (use unsigned); `drawUTF8` fonts are Latin-1, so no en dash or ellipsis; the panel
  inverted colours until the driver's `0x21` was changed to `0x20`.

## Hard lines

Never print, log or commit the LTA key or Wi-Fi password, and never commit `config.local.h` (personal location).
Restore stock firmware from the user's own dump: `esptool --port /dev/cu.usbmodem* write-flash 0 factory-flash-16MB.bin`
(README, "Restore the factory firmware"). Confirm with the user before reflashing anything other than this firmware.
