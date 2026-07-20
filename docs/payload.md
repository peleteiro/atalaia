# Watchlight payload format

This document is the canonical wire contract between Watchlight and a screen
provider. The machine-readable version is the [JSON Schema][schema], and a
complete valid document is available as an [example payload][example].

[schema]: ../schema/watchlight-payload.schema.json
[example]: ../examples/payload.json

## Transport

Watchlight makes an HTTPS request using the values compiled into
`src/secrets.h`:

| Item | Requirement |
| :--- | :--- |
| Method | `GET` |
| URL | `API_URL` |
| Authentication | `x-device-token: {API_TOKEN}` |
| Success | HTTP `200` with a JSON response body |

Any connection error, non-`200` response, or malformed JSON is a failed fetch.
It does not refresh the device's freshness timer.

## Document shape

This abbreviated example shows the structure. The `icon` matrix is shortened for
readability; use [`examples/payload.json`][example] for a schema-valid document.

```json
{
  "ts": 1784027753,
  "staleAfter": 1800,
  "screens": [
    {
      "id": "users",
      "text": "258k",
      "color": "#3b82f6",
      "icon": [
        ["#000000", "#000000", "… 6 more pixels"],
        ["… 7 more rows"]
      ]
    }
  ]
}
```

All top-level fields are required.

| Field | Constraints | Meaning |
| :--- | :--- | :--- |
| `ts` | Integer ≥ 0 | Unix timestamp, in seconds, when the provider read the source data. It is metadata; the firmware does not use it to calculate freshness. |
| `staleAfter` | Integer from 1 to 4,294,967 | Maximum seconds since the last accepted fetch before remote screens are replaced by the offline alert. |
| `screens` | Array containing 1–16 screens | Ordered remote screens. Watchlight displays them in array order, followed by its local screens. |

Every screen contains all four fields below. Additional fields are not part of
the contract.

| Field | Constraints | Meaning |
| :--- | :--- | :--- |
| `id` | String containing 1–64 characters | Stable identifier for the screen. Keep IDs unique within a payload. The current firmware does not render the ID, but tools and future firmware may use it to preserve identity across refreshes. |
| `text` | 0–15 printable ASCII characters | Value already formatted for display, such as `"258k"`. An empty string creates an icon-only screen. The practical visual limit is usually about eight characters and depends on glyph widths. |
| `color` | `#rrggbb` | Text color, with exactly six hexadecimal digits after `#`. Uppercase and lowercase digits are accepted. |
| `icon` | 8 rows × 8 colors | An 8×8 RGB bitmap. Every cell is a web-style `#rrggbb` color. |

## Icon encoding

An icon is a nested JSON array with exactly eight rows and exactly eight cells in
each row. Rows travel top-to-bottom and cells travel left-to-right, so a pixel is
addressed directly as `icon[y][x]`. Each cell uses the familiar web color syntax
`#rrggbb`; `#000000` is black and leaves that LED off.

```json
{
  "icon": [
    ["#000000", "#000000", "#3B82F6", "#3B82F6", "#3B82F6", "#3B82F6", "#000000", "#000000"],
    ["#000000", "#3B82F6", "#000000", "#000000", "#000000", "#000000", "#3B82F6", "#000000"],
    ["... six more rows, each with eight colors ..."]
  ]
}
```

The payload carries RGB888 values. The firmware converts them to the display
library's internal color representation before drawing.

## Freshness semantics

`staleAfter` measures time on the device since the last accepted fetch. It is not
compared with `ts`. Therefore, returning HTTP `200` asserts that the payload is
fresh enough to display. If the provider cannot obtain fresh source data, it must
return a non-success response instead of repeatedly returning old values.

After `staleAfter` elapses without an accepted fetch, Watchlight removes all
remote screens from the rotation and shows the offline alert. Device-local clock,
sensor, and low-battery screens remain available because they do not depend on
the endpoint.

Polling cadence is also device-local, not part of this contract. The firmware
uses `POLL_INTERVAL_FRESH_SECONDS` while the payload is fresh and
`POLL_INTERVAL_STALE_SECONDS` once it is stale; both live in `src/config.h`.

## Rotation

Rotation timing is not part of the JSON contract. The firmware uses
`SCREEN_ROTATION_SECONDS` from `src/config.h`, which defaults to 8 seconds and
applies to both remote and local screens.

## Validation

The [JSON Schema][schema] uses JSON Schema Draft 2020-12. It deliberately rejects
unknown fields, invalid colors, icon matrices that are not exactly 8×8,
non-ASCII display text, empty screen arrays, and arrays larger than the firmware's
16-screen limit. Validate payloads at the provider boundary; the ESP32 parser is
intentionally lightweight and may tolerate malformed values in ways that are not
part of this contract.

The matrix representation uses about 657 bytes per compactly serialized icon,
before the other screen fields. Keep responses small and within the 16-screen
limit so the complete document remains comfortable for the ESP32 to parse.
