# Setup page

On first boot, or after holding KEY for 3 seconds, the board opens its own Wi-Fi network and shows its name and a
one-time password. A phone that joins it gets a page to pick the home Wi-Fi and paste the LTA AccountKey.

## Sub-features

- `setup-screen` shows `AP_NAME` and an 8-character password on the panel.
- `setup-network` broadcasts `AP_NAME` as a Wi-Fi network.
- `setup-save` stores Wi-Fi and key on the board and returns to the card.

## How to get to it (user POV)

- First boot with no saved Wi-Fi.
- Hold KEY for 3 seconds at any time.

## Driving it with serial_ask.py and grab.py

Preconditions:

- Doctor passes. Do not submit the setup page: that replaces the owner's saved Wi-Fi and key.

- **Open it.** Run `python3 tools/grab.py verify-out/<run>/setup-screen.png k --wait 1 --delay 5`. The PNG shows
  `AP_NAME` and a password.
- **Network up.** Needs a person: `AP_NAME` appears in a phone's Wi-Fi list while the setup screen is up.
- **Close it.** Run `python3 .claude/skills/verify-daycard/scripts/serial_ask.py i --reset --wait 30`; with Wi-Fi
  saved, a restart never parks on setup, and `i` shows a non-zero `wifi` dBm.

## Gotchas

- The password changes every boot; never record it as expected text.
- `setup-save` and the first-boot path need a board with no saved Wi-Fi; on an owner's board they stay unverified
  unless the owner agrees to re-enter their Wi-Fi and key.
- The hold-KEY path needs a hand on the button; `k` stands in for it.
- A Mac shell can't confirm the network: macOS shows Wi-Fi names as `<redacted>` to apps without Location access.
