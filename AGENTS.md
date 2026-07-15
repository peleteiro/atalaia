# Atalaia — Agent Guide

Single source of truth for AI agents (Claude Code, Codex, Antigravity) working on
this repo. `CLAUDE.md` and `GEMINI.md` are symlinks to this file.

## What this is

**Atalaia** is content-agnostic firmware for a 32×8 pixel display (Ulanzi TC001 /
ESP32). It polls a JSON endpoint, rotates through the screens it returns, and
lights an alert when it can't reach fresh data. It knows nothing about *what* it
shows — every screen (text, color, 8×8 icon) arrives fully formed from the server.

Read `README.md` for the payload contract before touching rendering or fetch code.

## Golden rules

1. **Task runner is `mise`.** Never call `pio` directly in docs or scripts — use
   `mise run build` / `upload` / `monitor` / `clean`. Tasks are file-based scripts
   in `.config/mise/tasks/`, never inline in `.mise.toml`.
2. **Never commit `src/secrets.h`.** It holds WiFi passwords and the API token and
   is gitignored. Edits to the *shape* of secrets go in `src/secrets.example.h`.
3. **The device must never lie.** If data is older than `staleAfter`, show the
   offline glyph — never a stale number with a fresh face. Don't "fix" this by
   falling back to last-known values.
4. **Content stays on the server — except device-intrinsic screens.** Business
   icons and text come in the payload; never hardcode a business screen. The only
   baked-in screens are those rendering data no server could provide: the offline
   alert, and the **local hardware screens** (the DS3231 RTC clock, the SHT3x
   temp/humidity sensor). New data that isn't intrinsic to the device belongs on
   the wire, not in the firmware.
5. **Single file, readable.** The firmware lives in `src/main.cpp`. Keep it one
   screen tall in your head; a junior should follow it. Split only if it truly
   grows past readability.
6. **Comments in English.** Atalaia is a standalone, potentially-shared project —
   unlike the Biblebox monorepo (Portuguese). Keep prose and code comments English.
7. **Memory is bounded.** It's an ESP32. Prefer fixed arrays and streaming parses
   (`deserializeJson` from the stream) over buffering whole bodies. Respect
   `MAX_SCREENS`.
8. **Verify on hardware, don't claim.** Matrix orientation, pin numbers, and text
   fit can't be confirmed without flashing. Never state they work — say what needs
   checking on `mise run upload`.
9. **Never commit or push unless asked.** Make the edits and stop. Committing,
   staging for commit, or pushing happens only when the user explicitly requests
   it — don't do it on your own initiative after a change.

## Layout

| Path | Purpose |
| :--- | :--- |
| `src/main.cpp` | The firmware: WiFi, HTTPS fetch, JSON parse, render, rotation, offline glyph. |
| `src/secrets.example.h` | Template for credentials. Copy to `src/secrets.h` (gitignored). |
| `platformio.ini` | Board, framework, library deps. |
| `.config/mise/tasks/` | File-based mise tasks (build/upload/monitor/clean). |
| `.agents/` | Agent skills + rules. `.claude/` symlinks into here. |

## Workflow

```bash
mise run build     # compile (pio run)
mise run upload    # build + flash over USB
mise run monitor   # serial logs @ 115200
mise run upgrade   # update ESP32 platform + library deps
mise run clean     # wipe build artifacts
```

First build downloads the ESP32 toolchain (~minutes). Flashing needs the device on
USB; the port is auto-detected.

## Consumers

Atalaia is generic; Biblebox is one consumer. The Biblebox endpoint lives in the
monorepo at `apps/api/src/devices/atalaia.ts` (`GET /devices/atalaia`, auth via
`BIBLEBOX_ATALAIA_TOKEN`). Keep the contract in `README.md` and that endpoint in
sync — they're the two ends of the same wire.
