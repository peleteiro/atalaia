# Atalaia

A watchman for a 32×8 pixel display. It polls a JSON endpoint, rotates through the
screens it returns, and lights an alert when it can no longer reach fresh data.

Atalaia is **content-agnostic**: it knows nothing about the *business* it shows.
Every server screen arrives fully formed — text, color, and an 8×8 icon bitmap —
so adding, removing, or restyling one is a server change, never a reflash. The
firmware's only opinions are *how to draw* and *when to admit it's offline*.

The one exception is a pair of **local screens** for data no server could provide —
the on-board clock and temperature/humidity sensor. They ride in the same rotation
(see [Local screens](#local-screens)).

Target hardware is the **Ulanzi TC001** (ESP32 + WS2812B matrix), but any ESP32
board with a compatible 32×8 matrix works by adjusting the pins in `main.cpp`.

## The contract

Atalaia expects `GET {API_URL}` (sent with an `x-device-token` header) to return:

```json
{
  "ts": 1784027753,
  "staleAfter": 1800,
  "rotateSeconds": 8,
  "screens": [
    { "id": "users", "text": "258k", "color": "#3b82f6", "icon": "<384 hex chars>" }
  ]
}
```

| field | meaning |
| :--- | :--- |
| `ts` | epoch seconds when the numbers were read (for the server; the firmware tracks its own last-success clock) |
| `staleAfter` | seconds of no successful fetch before the offline glyph replaces the screens |
| `rotateSeconds` | seconds each screen is shown before advancing |
| `screens[].text` | short string, pre-formatted for the display (`"258k"`, not `"258482"`) |
| `screens[].color` | `#rrggbb` text color |
| `screens[].icon` | 8×8 bitmap: 64 pixels × `rrggbb`, row-major, left→right, top→bottom (= 384 hex chars). `000000` is an off pixel. |

The device renders the icon on the left 8×8 and the text — a small pixel font —
**centered** in the remaining 24px. Icons may use full RGB: gradients and
anti-aliased edges render fine (the panel is RGB per pixel), so a soft-edged 8×8
looks the part. The firmware never interprets the values — a screen is just pixels.

**Why the bitmap travels in the payload:** so a new screen or icon is a server
deploy, never a firmware flash. At ~1.6 KB per poll it's a rounding error.

## Buttons

The three top-edge buttons drive navigation locally, without touching the server:

| button | action |
| :--- | :--- |
| left | previous screen |
| middle — single click | pause / resume auto-rotation |
| middle — double click | **standby** (panel off, still running) — press to wake |
| middle — hold ~5s | **deep sleep** (powers down to save battery) — press to wake |
| right | next screen |

Stepping left or right resets the rotation timer, so a manual move never fights an
auto-advance. A single click freezes the current screen; new data still fetches in
the background and the frozen screen refreshes in place.

**Standby vs. deep sleep.** A double click blanks the panel but keeps the device
running, so the next middle press wakes it instantly. Holding ~5s enters ESP32
deep sleep — a real power-down for battery life; the middle button wakes it, which
reboots (reconnect WiFi, re-sync the clock, ~2–4s). Either way a press turns it
back on.

The pins (left `26` / middle `27` / right `14`) match the TC001. The piezo buzzer
(`15`) is held low at boot so it stays quiet.

## Local screens

Two screens render data the device reads itself, so they work with or without the
server (and even offline). The hardware was confirmed with an I2C scan: an **SHT3x**
temp/humidity sensor at `0x44` and a **DS3231** RTC at `0x68`, both on the I2C bus
(`SDA 21` / `SCL 22`).

- **Clock** — a 9px-wide white calendar page with a red header and the
  day-of-month **cut out of the white** (dark digits on the page), then `HH:MM`
  (24h) to the right with a colon that **fades out and in** each second. The RTC
  keeps time across reboots; whenever WiFi is up the firmware calibrates it from
  **NTP**. Timezone and NTP servers live in `src/config.h` (default: São Paulo,
  UTC-3). Until the clock is set it shows dashes.
- **Temperature / humidity** — one panel showing **both at once**: a thermometer
  with the temperature (°C) and, beside it, the humidity (%RH).

Numbers on these screens use the same hand-drawn 3×5 digits, for a consistent
pixel-clock look. They sit after the server screens in the rotation and are
reachable with the left/right buttons like any other screen.

## The offline glyph

If the last successful fetch is older than `staleAfter` (or none has happened
yet), Atalaia drops the server screens from the rotation and shows an amber warning
triangle in their place. This is deliberate: a panel showing yesterday's number
with a fresh face is worse than one that admits it's blind. The local screens
(clock, temp/humidity) keep rotating — they're never stale.

## Setup

1. Copy `src/secrets.example.h` → `src/secrets.h` and fill in your WiFi
   networks, `API_URL`, and `API_TOKEN`. `secrets.h` is gitignored.
   Non-secret knobs (clock timezone, NTP servers) live in `src/config.h`, which
   *is* committed — edit it in place.
2. Build and flash — the task runner is [`mise`](https://mise.jdx.dev) (wrapping
   [PlatformIO](https://platformio.org)):
   ```bash
   mise run build     # compile
   mise run check     # static analysis (cppcheck)
   mise run lint      # format the firmware (clang-format)
   mise run upload    # build + flash over USB
   mise run monitor   # serial logs at 115200
   ```
   The upload uses `upload_speed = 115200` (higher rates were unreliable on the
   test adapter). If a flash drops mid-transfer, retry — it's usually the USB cable.
3. To revert to stock, reflash the Ulanzi firmware (keep a backup `.bin`).

`API_ROOT_CA` is optional. Empty = skip TLS validation (`setInsecure`), which is
fine on trusted WiFi but lets a machine on the same network read the token. Pin
the endpoint's root CA when the display lives on untrusted WiFi.

## Hardware notes

Confirmed on the Ulanzi TC001. On a different board, check these first:

- **Matrix.** 32×8 on `MATRIX_PIN = 32`, wired **serpentine** — the code uses
  `NEO_MATRIX_ZIGZAG`. With `PROGRESSIVE` a compact drawing splits into a left half
  and a mirrored right half (the tell-tale of the wrong flag).
- **Buttons / buzzer.** Left `26`, middle `27`, right `14` (active-low, internal
  pull-ups); piezo buzzer `15`, held low so it stays silent.
- **Sensors.** I2C `SDA 21` / `SCL 22`; SHT3x temp/humidity at `0x44`, DS3231 RTC
  at `0x68`.
- **Deep-sleep wake.** The middle button wakes the ESP32 from deep sleep via `ext0`
  on GPIO 27. If a press won't wake it, that's the pin to check.
- **Text fit.** With the small font, values up to ~8 characters fit centered in the
  24px text area; longer ones crop on the right.

The clock digits, calendar, thermometer, and the °/% marks are hand-drawn bitmaps
in `main.cpp` — tweak them there for a different look. `mise run monitor` prints
boot, WiFi, fetch, NTP, sensor, and button events for troubleshooting.
