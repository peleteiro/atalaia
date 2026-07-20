---
trigger: always_on
---

# Rule: never commit secrets

`src/secrets.h` holds WiFi passwords and the API token. It is gitignored and must
stay that way. Never:

- commit `src/secrets.h` or remove it from `.gitignore`,
- paste its contents into other files, logs, or commit messages,
- hardcode credentials anywhere else in the tree.

`mise.local.toml` may hold `WOKWI_CLI_TOKEN` and is also gitignored. Only its
placeholder template, `mise.local.example.toml`, is committed.

Changes to the *shape* of secrets (new fields) go in `src/secrets.example.h`, with
placeholder values only.

The Wokwi target intentionally compiles the local `src/secrets.h` so it can test
the configured endpoint and token. Its `.pio/build/wokwi/` artifacts remain
gitignored and must never be committed or published as release assets.
