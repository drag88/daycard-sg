# Bus board

The bus board lists up to five services at the configured stop, each with its next three arrivals, bus type and
crowding. The header names the stop and its code; the services in `MY_BUSES` are in bold.

## Sub-features

- `bus-header` shows `BUS_STOP_NAME · BUS_STOP` from the config.
- `bus-rows` shows one row per `BUS_SERVICES` entry, in config order.
- `bus-mine` draws the `MY_BUSES` services in bold.
- `bus-live` fills arrival times from LTA DataMall.
- `bus-nokey` asks for the LTA key when none is saved.

## How to get to it (user POV)

- Tap KEY once from the card.
- Say "<CALL_NAME>, next bus", "show the buses" or "bus times".

## Driving it with serial_ask.py and grab.py

Preconditions:

- Doctor passes; an LTA key is saved for `bus-live`.

- **Header and rows.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py s --reset --wait 30`, then
  `python3 tools/grab.py verify-out/<run>/buses-sample.png p --wait 1`. The header reads
  the configured stop name and code, and the rows list `BUS_SERVICES` in order with the `MY_BUSES` ones bold.
- **What LTA returned.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py f --reset --wait 60`. The
  buses job reports a recent success, and the `buses at stop:` line lists every `BUS_SERVICES` entry.
- **Live arrivals.** Run `python3 tools/grab.py verify-out/<run>/buses-live.png p --wait 1`. At least one row shows
  minutes to arrival when buses are running (roughly 5:30 AM to midnight).

## Gotchas

- Overnight there are no buses; empty rows then are correct, not a failure.
- A wrong `BUS_STOP` code or a service that doesn't call there gives empty rows with no error on screen; cross-check
  the services against the stop pole or LTA.
- Without a saved key the board shows the "Add your LTA DataMall key" prompt instead of rows.
