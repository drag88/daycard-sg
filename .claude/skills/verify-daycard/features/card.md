# Main card

The card is the home view: a 12-hour clock with the date, three world clocks under it, a status row with the NEA
area's 2-hour forecast, UV and PSI, outside and indoor readings, a quote (or a notice) in the middle panel, and the
cat in the footer.

## Sub-features

- `card-clock` shows the time and date in Singapore time.
- `card-world` shows the three `WORLD_CLOCKS` labels from the config, each with a time.
- `card-weather` names `NEA_AREA` in the status row with the current forecast.
- `card-quote` fills the middle panel with a quote and its author.
- `card-cat` draws the cat in the footer.
- `card-notice` replaces the quote with a short notice for PSI over 100 or a heavy or thundery forecast, labelled
  `<NEA_AREA> FORECAST` for rain.

## How to get to it (user POV)

- It's what the board shows after boot.
- Tap KEY from the garden, or wait a minute on the bus, room or forecast view.
- Say "<CALL_NAME>, go home" or "<CALL_NAME>, main screen".

## Driving it with serial_ask.py and grab.py

Preconditions:

- Doctor passes; the expected `WORLD_CLOCKS` labels and `NEA_AREA` are read from the config that was flashed.

- **Layout with sample data.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py s --reset --wait 30`,
  then `python3 tools/grab.py verify-out/<run>/card-sample.png --wait 1`. The PNG
  shows the clock, the three configured world-clock labels in order, `NEA_AREA` in the status row, a quote with an
  author, and the cat.
- **Live data.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py f --reset --wait 60 > verify-out/<run>/card-feeds.txt`.
  Weather, UV, PSI and buses report a last success under a minute ago, and the nowcast period covers the current
  time. Then run `python3 tools/grab.py verify-out/<run>/card-live.png --wait 1`: the status row shows that
  forecast for `NEA_AREA`, and the clock matches Singapore time within a minute.
- **Notices.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py s --reset --wait 90` (sample data
  after the first live refresh, so it holds), then
  `python3 tools/grab.py verify-out/<run>/notice.png dddddddd --wait 108 --delay 2`. Among the shots, the middle panel
  reads `<NEA_AREA> FORECAST` / `Thundery showers in the 2-hour forecast`. The PSI notice comes first, the rain
  notice about two minutes later.

## Gotchas

- Right after boot the feeds haven't run yet; a live shot before about 40 s shows placeholders.
- Sample data replaces live readings until the next restart or the next refresh of each feed. Sample data loaded in
  the first minute after boot is overwritten almost at once, and its notices never show.
- World-clock times change with DST; check the label and that the hour offsets look right for the date, not a fixed
  time.
- During the morning routine (7-10 AM by default) a bus may be driving across the bottom.
