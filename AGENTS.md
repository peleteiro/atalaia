# Watchlight — Agent Guide

Single source of truth for AI agents (Claude Code, Codex, Antigravity 2) working
on this repo. `CLAUDE.md` is a symlink to this file; Codex and Antigravity read
it natively.

## What this is

**Watchlight** is content-agnostic firmware for a 32×8 pixel display (Ulanzi
TC001 / ESP32). It polls a JSON endpoint, rotates through the screens it returns,
and lights an alert when it can't reach fresh data. It knows nothing about *what*
it shows — every screen (text, color, 8×8 icon) arrives fully formed from the
server.

Read `docs/payload.md` and its JSON Schema before touching rendering or fetch
code. `README.md` provides the project overview.

## Golden rules

1. **Task runner is `mise`.** Never call `pio` directly in docs or scripts — use
   the `mise run build` / `check` / `lint` / `simulate-build` / `simulate` /
   `simulate:vscode` / `simulate:cli` / `upload` / `monitor` / `upgrade` / `clean`
   tasks. Tasks are file-based scripts in `.config/mise/tasks/`, never inline in
   `.mise.toml`.
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
6. **Comments in English.** Watchlight is a public, standalone project. Keep
   prose and code comments English.
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
| `src/main.cpp` | The firmware: WiFi, HTTPS fetch, JSON parse, render, rotation, buttons (standby / deep sleep), local clock + sensor screens, offline glyph. |
| `src/config.h` | Non-secret config (screen rotation, polling, clock, sensors). Committed — edit in place. |
| `src/secrets.example.h` | Template for credentials. Copy to `src/secrets.h` (gitignored). |
| `mise.local.example.toml` | Template for developer-local mise secrets such as `WOKWI_CLI_TOKEN`; copy to ignored `mise.local.toml`. |
| `docs/payload.md` | Canonical transport and JSON payload contract. |
| `schema/watchlight-payload.schema.json` | JSON Schema Draft 2020-12 for provider payloads. |
| `examples/payload.json` | Complete payload fixture that validates against the schema. |
| `platformio.ini` | Board, framework, library deps, `upload_speed`. |
| `diagram.json` / `wokwi.toml` | Wokwi circuit and firmware paths for visual/headless simulation. |
| `.vscode/` | Recommended firmware/Wokwi extensions, focused workspace settings, and mise-backed editor tasks. |
| `.config/mise/tasks/` | File-based mise tasks for build, checks, formatting, simulation, flashing, monitoring, upgrades, and cleanup. |
| `.agents/` | Shared agent skills, always-on rules, and the Antigravity MCP config. |
| `.claude/` | Claude project settings, format hook, and skills symlink. |
| `.codex/` | Codex sandbox, command policy, and format hook. |
| `.antigravity/` | Antigravity 2 project permissions. |

## Hardware (Ulanzi TC001), confirmed on device

Matrix 32×8 on GPIO 32, **serpentine** (`NEO_MATRIX_ZIGZAG` — not `PROGRESSIVE`).
Buttons active-low: left 26, middle 27, right 14. Piezo buzzer 15 (held low or it
squeals). I2C `SDA 21` / `SCL 22`: SHT3x temp/humidity `0x44`, DS3231 RTC `0x68`.
Middle button wakes deep sleep via `ext0` on GPIO 27. `upload_speed` is 115200
(higher was flaky). Full notes in `README.md`.

## Workflow

```bash
mise run build          # compile (pio run)
mise run check          # static analysis + Wokwi diagram validation
mise run lint           # format the firmware in place (clang-format)
mise run simulate-build # compile the Wokwi target
mise run simulate        # visual Wokwi simulator in VS Code (default)
mise run simulate:vscode # explicitly select the VS Code simulator
mise run simulate:cli    # headless Wokwi run; requires WOKWI_CLI_TOKEN
mise run upload         # build + flash over USB
mise run monitor        # serial logs @ 115200
mise run upgrade        # update ESP32 platform + library deps
mise run clean          # wipe build artifacts
```

First build downloads the ESP32 toolchain (~minutes). The Wokwi target uses the
local `src/secrets.h`, but connects through `Wokwi-GUEST`; its compiled artifact
must stay private. `mise run simulate` builds it and opens `diagram.json` in the
project's VS Code workspace by default; `mise run simulate:vscode` selects that
variant explicitly. Install the recommended extensions when prompted, then start
the Wokwi simulator. Headless simulation uses `mise run simulate:cli` and reads
`WOKWI_CLI_TOKEN` from the ignored `mise.local.toml`; `mise run clean` preserves
that file. Flashing needs the device on USB; the port is auto-detected.

## AI assistants

All three assistants share `AGENTS.md` and `.agents/skills/` as their source of
instructions. Claude reaches them through `CLAUDE.md` and `.claude/skills`; Codex
and Antigravity 2 discover both paths natively. `GEMINI.md` is intentionally absent
because it belongs to the legacy Gemini Code Assist setup, not Antigravity 2.

| Assistant | Project-scoped configuration |
| :--- | :--- |
| Claude Code | `.claude/settings.json` |
| Codex | `.codex/config.toml` and `.codex/rules/default.rules` |
| Antigravity 2 | `.antigravity/settings.json` and `.agents/mcp_config.json` |

The CLIs are installed by `mise`; use `mise exec -- claude`, `mise exec -- codex`,
or `mise exec -- antigravity` (`agy` is the short alias). Codex asks you to trust
the project on first use; then review and trust the project hook with `/hooks`.
For Antigravity 2, open this repository as a Project folder; its
`.agents/rules/` files are marked `always_on`.

Claude and Codex run `clang-format` on edited firmware files through their
`PostToolUse` hooks. Antigravity uses the IDE's format-on-save support, and
`lefthook` remains the final formatting guard for all three. The shared settings
contain mechanical guardrails for the canonical direct forms of `pio` /
`platformio`, force pushes, skipped commit hooks, and the destructive system
commands listed in each tool's settings. Permission matchers are prefix-based,
so the golden rules remain authoritative for reordered, wrapped, or aliased
equivalents. Codex also prompts before direct `git add`, `commit`, and `push`
commands. Rule 9 still applies: ordinary commits, staging, and pushes happen only
when the user explicitly asks.

There are currently no project-specific MCP servers, so
`.agents/mcp_config.json` contains an empty `mcpServers` object rather than
inheriting unrelated web or infrastructure servers from another repository.
