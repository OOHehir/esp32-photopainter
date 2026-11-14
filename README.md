# esp32-photopainter

A week of Google Calendar on a Waveshare ESP32-S3-PhotoPainter, the 7.3" six-colour
e-paper frame, built to run from a battery. The device fetches the calendar itself over
HTTPS, draws the next seven days & sleeps until the next half hour. No cloud account, no
API key, no phone application & no server of your own is needed to show the calendar.

The panel shows today's date, indoor temperature & humidity, battery level, seven day
columns from today, all-day & multi-day events as bars across the columns, & timed events
as coloured blocks, one colour per calendar. Events already over today are drawn in
outline. Events marked private in Google are left out.

## Hardware

| Part | Notes |
|---|---|
| Waveshare ESP32-S3-PhotoPainter | ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB octal PSRAM. 800 × 480 Spectra 6 panel, ~28 s refresh measured. AXP2101-compatible PMIC, SHTC3 sensor, KEY & BOOT buttons |
| 3.7 V LiPo cell, MX1.25 plug | Optional. Without one the board runs from USB |

Nothing to wire: pins are the board's own, in `components/app_epd/Kconfig` &
`components/app_board/app_board.c`.

## Requirements

- ESP-IDF v6.0.2. Earlier branches will not build
- A Google calendar's **Secret address in iCal format**: Google Calendar on a desktop
  browser → Settings → the calendar → *Integrate calendar*. Any iCalendar URL works
- For updates over the air, an HTTPS static file server; [`nas/README.md`](nas/README.md)
  documents the Synology one esp-ota & esp32-wx-station already use. Not needed to show
  the calendar
- Linux or macOS. Windows via WSL2, untested here

## Install & first run

```bash
cp sdkconfig.local.example sdkconfig.local     # then edit it
./tools/gen_signing_key.sh
./tools/gen_certs.sh <hostname> <ip>           # or copy server_certs/ca_cert.pem from a sibling project
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
idf.py set-target esp32s3 && idf.py build flash
```

Where:

- `sdkconfig.local` is gitignored & holds the site's settings: the provisioning salt, the
  time zone & optionally the calendars. Set `CONFIG_APP_CFG_AP_PASS_SALT` before
  commissioning a board; on the published default anyone who sees the setup network's
  name can compute its passphrase
- `gen_signing_key.sh` writes `keys/`. **Back it up: lose it & no board signed with it can
  ever be updated over the air again**
- `<hostname>` & `<ip>` are what the board will dial for firmware
- `activate_idf_v6.0.2.sh` is where EIM puts it on this machine; yours may differ

A board with no Wi-Fi credentials shows them on the panel:

```
Calendar setup
1.  On a phone, join this Wi-Fi network:   photopainter-a1b2c3
    Passphrase:                            K7MPQ2XR9WTF
```

Join it, pick your network, & the panel then shows the address of its web page. Open it,
& under *Calendars* add up to four: a name, a colour & the secret address. On a bench
board, `./tools/provision_nvs.sh -p /dev/ttyACM0 -s <ssid>` writes Wi-Fi credentials
over USB instead (passphrase from `$APP_WIFI_PASS`).

> The secret address is a password for the calendar. The device stores it in flash &
> never sends it back to the web page, which shows only the host. If one leaks, *Reset*
> it on the same Google settings page.

## Using it

| To | Do |
|---|---|
| Redraw now | Press KEY |
| Reach the web page, update or reflash | Hold KEY for 3 s: the board stays awake for 15 minutes |
| Flash a board that is asleep | Hold BOOT while pressing reset, or hold KEY first |
| Forget Wi-Fi & return to setup | `curl -X POST http://<board>/api/forget` while it is awake |

After power-on the board stays awake for two minutes before its first sleep.

## How a wake works

The board wakes on the half hour, skipping 23:00–06:00, & on KEY. Each wake joins Wi-Fi,
downloads each calendar, expands the seven days shown, renders the frame, & refreshes the
panel **only if the picture changed**: a refresh is 28 s of flashing & most of a wake's
energy. Temperature & battery readings are drawn but do not count as a change. An update
check runs once a day. Then it sleeps.

Measured on the bench board against a real Google calendar of 7 352 events back to 2010:

| | |
|---|---|
| Download | 584 KB gzip, 2.96 MB decoded. Google ignores date-range parameters & sends no ETag, so the whole calendar comes every wake |
| Fetch & parse | 9.8 s |
| Events in the week | 15, with 49 private events left out of the feed |

When a fetch fails, the last good week is drawn from a cache in flash & the footer says
*Offline – last updated HH:MM*. A response that is not iCalendar is a failure; it never
replaces the cache.

Settings worth knowing, under `idf.py menuconfig` or in `sdkconfig.local`:

| Option | Default | |
|---|---|---|
| `CONFIG_APP_CYCLE_INTERVAL_MIN` | 30 | Minutes between wakes |
| `CONFIG_APP_CYCLE_QUIET_START_H` / `_END_H` | 23 / 6 | No wakes between, local time |
| `CONFIG_APP_CYCLE_SLEEP_ON_USB` | y | `n` keeps a USB-powered board awake & reachable |
| `CONFIG_APP_CAL_TZ` | Ireland | POSIX TZ string the week is drawn in |
| `CONFIG_APP_CAL_HIDE_PRIVATE` | y | Leave out private & confidential events |
| `CONFIG_APP_CAL_SEED` | empty | Calendars to set on first boot, `name,colour,url` |
| `CONFIG_APP_EPD_ROTATE_180` | y | The stand holds the panel upside down |

## Updating over the air

The update path is esp-ota's: a signed image, a manifest on your server, four checks
before an image is kept, & a probation boot that reverts an image unable to reach the
server again. See [Publish an update](docs/publish-an-update.md).

## Components

| Component | Responsibility |
|---|---|
| `app_cycle` | One wake: bring-up, fetch, draw, update check, sleep |
| `app_cal` | Calendar list, HTTPS fetch with streaming gzip, offline cache |
| `app_cal` `cal_*.c` | iCalendar parser, recurrence expansion, time zones. Pure C |
| `app_ui` | The week layout. Events in, drawing calls out. No I/O |
| `app_display` | Render, hash, refresh only on change |
| `app_epd` | The Spectra 6 panel controller |
| `app_gfx` | Surfaces, primitives, 1-bit Latin-1 fonts. No I/O |
| `app_board` | PMIC, battery, SHTC3, KEY |
| `app_core`, `app_cfg`, `app_diag`, `app_trust`, `app_net`, `app_httpd`, `app_ota`, `app_led` | From esp-ota: bring-up, settings, diagnostics, trust anchors, Wi-Fi, web page, updates, LED |

Comments in the inherited components cite `PLAN §…`; that is esp-ota's `PLAN.md`.

## Tests

```bash
./tools/run_tests.sh
# → 569 checks, 0 failed
# → all tests passed
```

Host-only, under AddressSanitizer: the iCalendar parser against Google- & Outlook-shaped
feeds (time zones, daylight-saving transitions, every recurrence form Google writes,
moved & cancelled instances, private events, every chunk size from 1 to 64 bytes), the
layout's band seams, the wake schedule, & esp-ota's update & rollback policy. The feed
fixtures are hand-written, not recorded from Google.

```bash
make -C test/host preview PREVIEW_DIR=/tmp && xdg-open /tmp/week.ppm
```

Renders the week, offline, setup & no-calendar screens to files.

## What it does not do

- **No per-event colours.** Google's iCal export carries none; each calendar has one. A
  feed with RFC 7986 `COLOR` lines is honoured
- **No touch, no interaction** beyond KEY. It is a display
- **No authentication on the web page**, which is up whenever the board is awake: each
  wake's few seconds, two minutes after power-on, 15 minutes after a KEY hold or during
  setup, & always on USB with `CONFIG_APP_CYCLE_SLEEP_ON_USB=n`. Anyone on the network can
  change the calendars then
- **No secure boot or flash encryption.** Images are signed; the calendar URLs sit
  unencrypted in flash, so anyone holding the board can read them
- **Battery life is not yet measured.** The bench board has no cell & runs with
  `CONFIG_APP_CYCLE_SLEEP_ON_USB=n`, so deep sleep & the KEY wake have not yet run on
  hardware

## Getting help & contributing

Open an issue with the serial log from boot. Run `./tools/run_tests.sh` &
`./tools/build_matrix.sh` before proposing a change.

## Licence

MIT — see [LICENSE](LICENSE). The font tables in `components/app_gfx/font_*.c` derive
from DejaVu & carry their own notice: [`FONT-LICENCE.txt`](components/app_gfx/FONT-LICENCE.txt).
