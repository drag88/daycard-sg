# Focus and garden

BOOT starts a 25-minute focus session with a growing plant, then a five-minute break. Each completed work phase
plants a flower in the permanent garden, which keeps lifetime, monthly and hours counts across restarts.

## Sub-features

- `focus-start` opens the focus view with a 25:00 countdown.
- `focus-stop` ends a session early without earning a flower.
- `garden-view` shows the flower count and the latest flowers.
- `garden-persist` keeps counts across a restart.

## How to get to it (user POV)

- Tap BOOT to start; hold BOOT for 2 seconds to stop.
- Tap KEY to reach the focus view and then the garden.
- Say "<CALL_NAME>, focus", "<CALL_NAME>, stop", "<CALL_NAME>, show garden".

## Driving it with serial_ask.py and grab.py

Preconditions:

- Doctor passes. Record the starting counts first:
  `python3 .claude/skills/verify-daycard/scripts/serial_ask.py G > verify-out/<run>/garden-before.txt`.

- **Start and stop.** Run `python3 tools/grab.py verify-out/<run>/focus-running.png r --wait 1 --delay 3`. The PNG
  shows the focus view counting down from 25:00. Then run
  `python3 .claude/skills/verify-daycard/scripts/serial_ask.py FRF`. The first `F` reports an active phase, the
  second reports it stopped.
- **Idle hint.** From the card, run `python3 tools/grab.py verify-out/<run>/focus-idle.png pppppd --wait 1 --delay 1.5`
  (five KEY taps: buses, room, forecast, photos, focus). The footer reads `Tap BOOT or say '<CALL_NAME> focus' to
  begin.` with the name capitalised.
- **Garden.** Run `python3 tools/grab.py verify-out/<run>/garden.png g --wait 1 --delay 3`. The count on screen
  matches `total` in `garden-before.txt`.
- **No flower for a cancelled session.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py G`. `total`
  is unchanged from `garden-before.txt`.

## Gotchas

- A restart cancels a running session; don't pass `--reset` between start and stop.
- The garden stays open once selected; reset before driving another feature.
- Earning a flower takes the full 25 minutes; prove `garden-persist` with the count before and after a restart
  rather than by waiting.
- The focus view shows the owner's daily intention when one is set; don't keep that screenshot as public evidence.
