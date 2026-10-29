# Voice phrases

The board listens all the time for whole phrases that start with the configured name (`CALL_NAME`, "flubber" by
default) and switches views or starts focus offline. Nothing leaves the board.

## Sub-features

- `voice-phrases` holds every phrase from `voice.h`, each starting with `CALL_NAME`.
- `voice-listen` keeps the recogniser running (listens per minute stays near 10).
- `voice-act` performs the command a person says.

## How to get to it (user POV)

- Say "<CALL_NAME>, next bus" (or any phrase in the README's Voice table) within a couple of metres of the board.

## Driving it with serial_ask.py and grab.py

Preconditions:

- Doctor passes with `voice on`.

- **Phrases.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py C > verify-out/<run>/voice-phrases.txt`.
  Every `cmd` line's quoted phrase starts with `CALL_NAME`, every phrase has phonemes (not `(none)`), and the list
  ends with `cmd end`.
- **Listening.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py i`. `voice on`, and the
  listens count is above zero.
- **Acting.** Needs a person: run `python3 tools/voicetest.py 3` and have them say the prompted phrases. Each round
  reports what was heard and how fast.

## Gotchas

- Computer speakers don't reach the board's mic well; `voice-act` needs a person speaking, so report it unverified
  when no one is there.
- A new `CALL_NAME` that MultiNet can't spell into phonemes shows `(none)` in `C`; pick another name.
- Wi-Fi reconnect storms starve the recogniser; fix Wi-Fi before blaming voice.
