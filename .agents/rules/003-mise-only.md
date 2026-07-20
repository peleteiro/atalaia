---
trigger: always_on
---

# Rule: mise for tasks, never raw pio

Build, check, format, simulate, flash, and monitor go through `mise`. The core
commands are `mise run build`, `mise run check`, and `mise run lint`. Simulation
uses `mise run simulate-build`, `mise run simulate` (VS Code by default),
`mise run simulate:vscode`, or `mise run simulate:cli`. Hardware upload and logs
use `mise run upload` and `mise run monitor`. Cleanup uses `mise run clean`. Tasks
are file-based scripts in `.config/mise/tasks/`; never define tasks inline in
`mise.toml`. When documenting or scripting, reference the `mise run …` form, not
`pio …` or `platformio …` directly.
