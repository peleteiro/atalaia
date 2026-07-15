#pragma once

// Non-secret device configuration. Unlike secrets.h, this file IS committed — it
// holds no credentials, only presentation/behavior knobs you may want to tweak.

// POSIX timezone string for the clock. Default: São Paulo / Brazil (UTC-3, no DST).
// Change to match where the display lives, e.g.:
//   "PST8PDT,M3.2.0,M11.1.0"          US Pacific
//   "CET-1CEST,M3.5.0,M10.5.0/3"      Central Europe
//   "<-03>3"                          fixed UTC-3 (São Paulo)
#define TIMEZONE "<-03>3"

// NTP servers the RTC calibrates against whenever WiFi is up.
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"
