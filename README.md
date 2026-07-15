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

The device renders the icon on the left 8×8 and the text right-aligned in the
remaining 24px. It never interprets the values — a screen is just pixels.

**Why the bitmap travels in the payload:** so a new screen or icon is a server
deploy, never a firmware flash. At ~1.6 KB per poll it's a rounding error.

## Buttons

The three top-edge buttons drive navigation locally, without touching the server:

| button | action |
| :--- | :--- |
| left | previous screen |
| middle — single click | pause / resume auto-rotation |
| middle — double click | standby (panel off) / press again to wake |
| right | next screen |

Stepping left or right resets the rotation timer, so a manual move never fights an
auto-advance. Pausing freezes the current screen; new data still fetches in the
background and the frozen screen refreshes in place. A double click blanks the
panel and stops polling; the next middle press wakes it and refetches. The pins
(`26` / `27` / `14`) match the TC001 — verify on hardware if a press does nothing
on first flash.

## Local screens

Two screens render data the device reads itself, so they work with or without the
server (and even offline). The hardware was confirmed with an I2C scan: an **SHT3x**
temp/humidity sensor at `0x44` and a **DS3231** RTC at `0x68`.

- **Clock** — a little calendar block with the day-of-month on the left, `HH:MM`
  (24h) on the right. The RTC keeps time across reboots; whenever WiFi is up the
  firmware calibrates it from **NTP**. Timezone and NTP servers live in
  `src/config.h` (default: São Paulo, UTC-3). Until the clock is set it shows `--:--`.
- **Temperature / humidity** — one rotation slot that alternates every ~2s between
  a thermometer + °C and a droplet + %RH.

These sit after the server screens in the rotation and are reachable with the
left/right buttons like any other screen.

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
2. Flash with [PlatformIO](https://platformio.org):
   ```bash
   pio run -t upload    # build + flash over USB
   pio device monitor   # serial logs at 115200
   ```
3. To revert to stock, reflash the Ulanzi firmware (keep a backup `.bin`).

`API_ROOT_CA` is optional. Empty = skip TLS validation (`setInsecure`), which is
fine on trusted WiFi but lets a machine on the same network read the token. Pin
the endpoint's root CA when the display lives on untrusted WiFi.

## Verify on first flash

These can't be confirmed without the hardware — check them when you flash:

- **Matrix orientation.** If the image is mirrored or rotated, adjust the
  `NEO_MATRIX_*` flags in `main.cpp`. The current flags target the TC001.
- **Data pin.** `MATRIX_PIN = 32` is the TC001's; other boards differ.
- **Text fit.** Values up to 4 characters fit; longer ones clip on the right.
  Scrolling long values is a natural next step.
- **Local-screen glyphs & fit.** The thermometer, droplet, and calendar bitmaps,
  the small clock font (TomThumb), and the degree symbol (`\xF7` in the default
  font) are all eyeballed — tweak them in `main.cpp` if they read wrong.
- **Sensor bus.** `I2C_SDA = 21` / `I2C_SCL = 22` and the SHT3x/DS3231 addresses
  match this unit's I2C scan; a different board may differ.
