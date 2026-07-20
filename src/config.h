#pragma once

// Non-secret device configuration. Unlike secrets.h, this file IS committed — it
// holds no credentials, only presentation/behavior knobs you may want to tweak.

// Seconds each screen stays visible before auto-rotation advances. Applies to
// both server-provided and local screens.
#define SCREEN_ROTATION_SECONDS 8

// Payload polling cadence. While the current payload is fresh, conserve network
// and battery by polling every 15 minutes. Once it becomes stale (or before the
// first successful fetch), retry every minute so recovery is quick.
#define POLL_INTERVAL_FRESH_SECONDS (15UL * 60UL)
#define POLL_INTERVAL_STALE_SECONDS (1UL * 60UL)

// POSIX timezone string for the clock. Default: São Paulo / Brazil (UTC-3, no DST).
// Change to match where the display lives, e.g.:
//   "PST8PDT,M3.2.0,M11.1.0"          US Pacific
//   "CET-1CEST,M3.5.0,M10.5.0/3"      Central Europe
//   "<-03>3"                          fixed UTC-3 (São Paulo)
#define TIMEZONE "<-03>3"

// NTP servers the RTC calibrates against whenever WiFi is up.
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

// The temp/humidity sensor sits by the ESP32 and LEDs, so it reads several degrees
// high. This offset (°C) is added to the raw reading before display — a best guess
// until you compare against a real thermometer, then tweak it. (Higher brightness =
// more self-heating = a more negative offset.)
#define TEMP_OFFSET_C (-7.0f)

// Temperature unit shown on the panel: 0 = Celsius (default), 1 = Fahrenheit. The
// offset above is always in °C and is applied before any Fahrenheit conversion.
#define TEMP_FAHRENHEIT 0
