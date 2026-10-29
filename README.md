# daycard

Firmware that turns a Waveshare ESP32-S3-RLCD-4.2 (a 400×300 reflective black-and-white LCD) into a Singapore desk
card. It shows a 12-hour serif clock, NEA's 2-hour rain forecast for your area with a live weather animation,
outside temperature, humidity and feels-like, UV and PSI, indoor temperature and humidity, battery level, three
world clocks, a new quote every 30 minutes from Wikiquote, and live LTA bus arrivals at your stop. In the morning
a bus drives across the bottom of the screen when one is a minute out. A small tabby cat lives on the card and
answers to offline voice commands, a focus timer grows a garden, and in quiet evenings the card becomes a photo
frame.

Buses and weather come from Singapore's LTA DataMall and NEA (data.gov.sg), so outside Singapore you'd get the
clock, quotes, room readings, focus timer and photos, but no buses or weather.

## What you need

- A [Waveshare ESP32-S3-RLCD-4.2](https://www.waveshare.com/esp32-s3-rlcd-4.2.htm) and a USB-C data cable.
- A free LTA DataMall AccountKey ([request one](https://datamall.lta.gov.sg/content/datamall/en/request-for-api.html)).
- 2.4 GHz Wi-Fi.
- Optional: a microSD card (FAT32, 32 GB or smaller) for photos, the 24-hour room chart and logs, and an 18650
  cell to run on battery.
- A computer with [arduino-cli](https://arduino.github.io/arduino-cli/latest/installation/) (macOS:
  `brew install arduino-cli`). The first build downloads about 1.7 GB of ESP32 toolchain (6 GB once unpacked).

## Quick start

1. Fork this repo, then clone it into a folder named `daycard`, since arduino-cli needs the folder to match
   `daycard.ino`:

   ```sh
   git clone https://github.com/<your-account>/daycard-sg.git daycard && cd daycard
   ```

   A GitHub ZIP unpacks as `daycard-sg-main`; rename that folder to `daycard`.
2. Open `config.h` and set your bus stop, its services, your NEA area and region, weather stations, world clocks
   and the name your cat answers to. Each setting says where to find its value. To keep your settings out of git,
   copy `config.h` to `config.local.h` and edit that instead; the build prefers it and `.gitignore` skips it.
3. Optional but wise: back up the factory firmware first (see [Restore the factory firmware](#restore-the-factory-firmware)).
4. Plug the board in, find its port with `arduino-cli board list` (`/dev/cu.usbmodem…` on a Mac, `/dev/ttyACM0` on
   Linux, `COM3` or similar on Windows), then build and flash from this folder:

   ```sh
   arduino-cli compile --build-property upload.maximum_size=6291456 .
   arduino-cli upload -p /dev/cu.usbmodem* .
   ```

   `sketch.yaml` pins the board settings, the ESP32 core (3.3.12) and the libraries (U8g2, ArduinoJson 7,
   WiFiManager 2.0.17), so the first compile fetches exactly those.
5. Give it about 30 seconds after flashing. On first boot the screen shows a Wi-Fi network (`Daycard-Setup`) and a
   password that changes every boot. Join it from your phone, pick your Wi-Fi, paste your LTA AccountKey and save.
   Both stay on the board only. Hold KEY for 3 seconds to open this setup page again later.

Reflashing keeps the saved Wi-Fi and key. Over USB, sending `K<key>` and a newline also sets the key.

## Using it

Tap KEY to step through the views: card, bus board (next three arrivals with crowding), room history (24-hour
temperature and humidity chart, logged to the memory card), forecast, photos, focus timer and garden. Bus, room and
forecast return after a minute, to focus if a session is running, otherwise to the card. KEY takes you from the
garden back to the card.

Under the big clock are three world clocks (London, New York and Tokyo by default), each with its own
daylight-saving rule. Change them in `config.h`.

BOOT starts a 25-minute focus session followed by a five-minute break. A plant grows into a flower, with a soft
chime at each boundary. Tapping again returns to the running session without restarting it; holding BOOT for two
seconds stops it. Sessions run while you browse other views and keep automatic photos away; a restart cancels the
session.

The permanent garden gets one flower for every completed 25-minute work phase, even if you stop during the break.
Cancelled work earns none. It keeps your lifetime flower count, this month's count and total focus hours, and draws
the latest 30 flowers with the cat beside them. Counts survive restarts: a snapshot is saved on the board and
appended to `daycard/garden.log` on the card, with versioned checksums so incomplete writes are skipped on recovery.
Without a card the board still keeps your flowers.

The cat (Flubber by default; rename it with `CALL_NAME`) sits on the card's work and evening footer and in the focus
garden. It blinks, gets sleepy late at night, carries an umbrella under a fresh rainy forecast, and celebrates the
switch from focus to break. Every 90 seconds it takes a short stroll, then stretches, washes a paw or naps before
returning to its spot. It stays settled during focus, shelters under rain and naps late at night. Short captions
follow what it's doing; the focus hint appears for up to eight seconds every five minutes while it sits normally.

The day follows a routine, in Singapore time, set by `MORNING_HOUR`, `WORK_HOUR` and `EVENING_HOUR`. From 7 to
10 AM buses run along the bottom; from 10 AM to 6 PM the card favours the clock and focus controls; from 6 PM to
7 AM, after `IDLE_MINUTES` (5) of quiet, the card becomes the photo frame. Talking, any sound well above the room's
background, a button or a voice command brings the card back; sound stands in for motion, since the board has no
motion sensor. Photo and focus views you open yourself stay until you leave them.

The middle panel normally shows a quote, alternating every 20 seconds with today's intention when one is set.
Fresh 24-hour PSI of 101–200, heavy or thundery forecasts, connection trouble and overdue feeds trigger brief
notices: 12 seconds, then normal content returns, and another notice two minutes after the previous one started
while conditions persist. Several notices take turns. PSI of 201 or more stays up until it improves or goes stale.
PSI categories follow [NEA's guide](https://www.haze.gov.sg/docs/default-source/posters/haze-pm-psi-guide-a4-yoca-english_18-oct.pdf).
Feeds count as overdue after 25 minutes for weather, 90 seconds for buses (morning only) and 65 minutes for PSI.

### Daily intention

`python3 tools/intention.py "Finish one useful thing"` sends a note over USB: up to 56 printable ASCII characters,
or `""` to clear it. Add `--port /dev/cu.usbmodem…` when more than one board is attached. The note shows on the card
when no alert has priority and on the focus view throughout a session. It survives restarts, expires at midnight
Singapore time, and isn't echoed to serial logs. The helper checks the firmware supports it before sending any text.
The clock must be set first.

### Photos

The frame shows JPEGs from the `photos` folder on the memory card, a new one every `PHOTO_MINUTES` (5).
`python3 tools/nature.py out && python3 tools/upload.py out/*.jpg` fetches 227 hand-picked Unsplash nature photos
(via Lorem Picsum, credited on screen) and copies them over USB; the id list is in the script. Your own JPEGs go in
the same folder, or over USB with `tools/upload.py`. The board only decodes baseline JPEGs: convert iPhone HEICs
first (`sips -s format jpeg -Z 1600 IMG.HEIC --out IMG.jpg` on a Mac); progressive JPEGs are skipped.

### Memory postcards

Add a personal photo with its own date and caption:

```sh
python3 tools/memory.py photo.heic --date 2024-09 --caption "Beach trip"
```

Use `YYYY-MM` or `YYYY-MM-DD` and 1–56 printable ASCII characters. The helper fixes orientation, resizes to at most
1200 pixels and writes a baseline JPEG with Pillow; HEIC/HEIF conversion uses macOS `sips`. The frame alternates
postcards with nature photos. Every other postcard turn favours the same month, exact day first, labelled "On this
day"; the other turns draw from the whole collection. The date and caption sit on a white strip along the bottom.

With `--card /Volumes/DAYCARD` (or any folder) the helper writes to a mounted card instead of USB. Each postcard is
a JPEG plus a matching `.jpg.json` sidecar in `memories/` (for example `{"date":"2024-09","caption":"Beach trip"}`);
keep both together. Uploads publish the sidecar last, so an interrupted transfer stays out of the slideshow, and
sending the same postcard twice doesn't duplicate it.

### Quotes

Every `QUOTE_MINUTES` (30) the board reads one author's Wikiquote page from `QUOTE_AUTHORS` and picks a sourced line
that fits two lines of the card and hasn't shown in about ten days; an author who has run out of short lines repeats
the one shown longest ago. Each is logged to `daycard/quotes.log` on the card. Until the first one arrives after a
restart, the card shows 49 built-in quotes.

The room log is `daycard/room.csv` on the card (epoch, raw temperature, raw humidity, battery volts), one line a
minute. The chart applies the current calibration, so recalibrating fixes the history too.

## Voice

Say the whole phrase, starting with the name. It runs offline on Espressif's ESP-SR (MultiNet 7, English); no audio
leaves the board.

| Say | Does |
|---|---|
| Flubber, next bus / show the buses / bus times | bus board |
| Flubber, show the forecast / will it rain / weather | forecast |
| Flubber, show the room / show room / room temperature | room chart |
| Flubber, show photos / photo frame | photo frame |
| Flubber, go home / main screen | the card |
| Flubber, next quote | fetch a new quote now |
| Flubber, focus | start a focus session, or return to the running one |
| Flubber, stop | stop the focus session |
| Flubber, show garden / my garden | the garden |

Swap "Flubber" for your own `CALL_NAME`. There's no trained wake word, so the recogniser listens continuously for
whole phrases with one mic, and the name does the wake word's job. Action phrases are short and distinct on purpose:
long shared openings pulled unclear speech into the wrong command. Phrases live in `voice.h`; `python3
tools/voicetest.py 3` runs prompted rounds and reports what was heard and how fast. Test with a person speaking;
laptop speakers usually don't reach the mic.

## Self-healing

It's meant to run unattended. Every 30 s it rejoins Wi-Fi if it dropped (after a 20 s grace), rebuilds the
connection if no feed has succeeded for 20 minutes, and restarts after 45 minutes without data, when memory runs
low, and nightly at 4 AM. A task watchdog restarts it if the display loop or network task stops for 2 minutes. With
a network saved, a restart never parks on the setup screen. The PCF85063 real-time clock is set from NTP and keeps
time across restarts while offline. Every boot and restart is logged with its reason to `daycard/restarts.log`.

## Build notes

- `partitions.csv` replaces the `esp_sr_16` partition table with one 6 MB app instead of two 3 MB OTA slots,
  because the speech recogniser alone is about 2 MB. `esp_sr_16` is still selected so the upload also writes the
  speech models to 0xC10000, and `--build-property upload.maximum_size=6291456` lifts the 3 MB size check. Leave all
  three as they are.
- `LoopCore=0` puts the display on core 0 so the recogniser has core 1 to itself; sharing a core, it fell to about
  70% of real time and missed words.
- On Apple Silicon Macs without Rosetta, arduino-cli's bundled `ctags` (an Intel binary) can't run and every compile
  fails. Install Rosetta once: `softwareupdate --install-rosetta --agree-to-license`.
- The Arduino IDE can't pass the size override, so build with arduino-cli.

## Restore the factory firmware

Before your first flash, save the whole 16 MB flash with [esptool](https://github.com/espressif/esptool)
(`pip install esptool`, or `uvx esptool` with uv):

```sh
esptool --port /dev/cu.usbmodem* read-flash 0 0x1000000 factory-flash-16MB.bin
```

To go back to stock later:

```sh
esptool --port /dev/cu.usbmodem* write-flash 0 factory-flash-16MB.bin
```

## Changing things

- Location, buses, world clocks, the cat's name, routine hours, timings, quote authors and calibration are all in
  `config.h`.
- Indoor temperature reads high because the SHTC3 sensor sits beside the warm ESP32. Put a room thermometer next to
  the card, read the serial command `i` at the same moment, set `TEMP_OFFSET_C` = meter − raw, flash, then set
  `RH_OFFSET` = meter humidity − shown humidity. Get the temperature right first: humidity is corrected for it.
- Built-in quotes and type: edit `tools/quotes.txt`, then
  `uv run --no-project --with pillow python tools/make_assets.py` to re-render `assets.h` (clock digits, built-in
  quotes, and the Newsreader letters live quotes are set in). On a Mac with Homebrew, prefix it with
  `DYLD_LIBRARY_PATH=/opt/homebrew/lib` so Pillow uses raqm layout, which the committed bitmaps were made with;
  without it every quote shifts by a pixel or two.
- New view: add it to `enum View`, write a `draw…` function, and add it to the render chain in `loop()`.
- New voice phrase: add it to `VOICE_COMMANDS` in `voice.h` and handle it in `handleVoice()`.

## Development

Host tests, no board needed:

```sh
c++ -std=c++17 tools/test_logic.cpp -o /tmp/t && /tmp/t                 # parsing, timing, DST, quotes, battery
c++ -std=c++17 tools/test_keepsakes.cpp -o /tmp/keepsakes && /tmp/keepsakes  # garden snapshots, postcard dates
python3 -m unittest discover -s tools -p test_memory.py                   # photo preparation, USB protocol
```

The board talks over USB serial with single-letter commands: `i` status, `f` feed ages, `F` focus status, `G`
garden counts, `C` the phrases the recogniser holds, `p` next view, `s` sample data, `d` framebuffer dump, and more
in `handleSerial()` in `daycard.ino`. `python3 tools/grab.py shot.png [commands] --wait 1` screenshots the real
framebuffer: `s` loads sample data (it lasts until the next restart or feed refresh), `w` steps through the weather
scenes, `b` runs a bus and `d` takes a shot.

If you use Claude Code, `.claude/skills/daycard` holds the working knowledge (pins, debugging, gotchas) and
`.claude/skills/verify-daycard` is a scripted way to flash a build and prove it on the board.

## License

MIT, see [LICENSE](LICENSE). The display driver (`ST7305_U8g2.*`) is Waveshare's, under Apache 2.0; the fonts in
`tools/fonts/` are under the SIL Open Font License.
