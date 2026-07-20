---
name: build
description: Build, check, format, simulate, flash, and monitor Watchlight via mise
---

# build

Compile, check, format, simulate, flash, and observe the Watchlight firmware.
Always go through `mise`, never raw `pio` or `platformio`.

## Commands

- `mise run build` — compile only. First run downloads the ESP32 toolchain and
  libraries (several minutes); later runs are fast.
- `mise run check` — run PlatformIO static analysis and validate `diagram.json`.
- `mise run lint` — format the tracked firmware sources with clang-format.
- `mise run simulate-build` — compile the Wokwi target. It uses the local
  `src/secrets.h`, with WiFi association redirected to `Wokwi-GUEST`.
- `mise run simulate` — run the default simulator, currently
  `mise run simulate:vscode`.
- `mise run simulate:vscode` — compile the Wokwi target, open the project in VS
  Code, and select `diagram.json`. Install the workspace's recommended extensions
  and start the Wokwi simulator from its visual editor.
- `mise run simulate:cli` — build and run headlessly in Wokwi; requires a local
  `WOKWI_CLI_TOKEN` (copy `mise.local.example.toml` to `mise.local.toml`). For the
  visual simulator, prefer `mise run simulate`.
- `mise run upload` — build and flash over USB. The device must be connected; the
  serial port is auto-detected.
- `mise run monitor` — open the serial monitor at 115200 baud to read logs.
- `mise run upgrade` — update the ESP32 platform and firmware libraries.
- `mise run clean` — remove build artifacts while preserving `mise.local.toml`.

## After flashing — verify on hardware

The compiler can't confirm these; check them the first time you flash real hardware:

- **Matrix orientation.** Mirrored or rotated output → adjust the `NEO_MATRIX_*`
  flags in `src/main.cpp`.
- **Data pin.** `MATRIX_PIN` defaults to the Ulanzi TC001's GPIO 32.
- **Text fit.** Up to 4 characters fit beside the icon; longer values clip.
- **Offline glyph.** Pull the network or point `API_URL` at a dead host and
  confirm the amber alert replaces the screens after `staleAfter`.

## Prerequisite

`src/secrets.h` must exist (copy from `src/secrets.example.h`). Without it the
build fails on a missing include — that's expected, not a bug.

The Wokwi artifact contains the compiled API configuration from `secrets.h` and
must stay private. The simulator models the matrix, buttons, buzzer pin, battery
ADC, and basic RTC behavior (using a DS1307 stand-in for the DS3231), but not the
TC001's exact SHT3x part or DS3231-specific behavior. It does not replace final
checks on physical hardware.
