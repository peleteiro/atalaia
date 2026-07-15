// Atalaia — a watchman for a 32x8 pixel display.
//
// It polls a JSON endpoint, rotates through the screens it returns, and lights an
// alert when it can no longer reach fresh data. The device is deliberately dumb:
// every screen arrives fully formed (text, color, 8x8 icon bitmap), so adding or
// changing a screen is a server-side change — never a reflash.
//
// The one thing the firmware refuses to do is lie: if the last successful fetch is
// older than the server-declared `staleAfter`, it shows the offline glyph instead
// of a stale number wearing a fresh face.

#include <Adafruit_GFX.h>
#include <Adafruit_NeoMatrix.h>
#include <Adafruit_SHT31.h>
#include <ArduinoJson.h>
#include <Fonts/TomThumb.h>
#include <HTTPClient.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <time.h>

#include "config.h"
#include "secrets.h"

// ---- Hardware (Ulanzi TC001) -----------------------------------------------
// The matrix data pin and orientation flags below match the TC001. On a different
// board, or if the image comes out mirrored/rotated on first flash, this is the
// first place to adjust.
static const uint8_t MATRIX_PIN = 32;
static const uint8_t BRIGHTNESS = 40;  // 0-255; the panel is bright up close.

Adafruit_NeoMatrix matrix(32, 8, MATRIX_PIN,
                          NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_PROGRESSIVE,
                          NEO_GRB + NEO_KHZ800);

// The three top-edge buttons, active-low (internal pull-up, pressed = LOW). Pins
// match the TC001; verify on hardware if a press does nothing on first flash.
// Left/right step through screens; the middle button pauses/resumes rotation.
enum Btn { BTN_LEFT, BTN_MID, BTN_RIGHT, BTN_COUNT };
static const uint8_t BTN_PINS[BTN_COUNT] = {26, 27, 14};
static const uint16_t DEBOUNCE_MS = 30;
static const uint16_t DOUBLE_CLICK_MS = 350;  // window to catch a second middle click

// The piezo buzzer. Left floating it squeals, so we hold it low at boot (the way
// the stock firmware does) and otherwise never touch it.
static const uint8_t BUZZER_PIN = 15;

// ---- On-board sensors (Ulanzi TC001) ---------------------------------------
// One I2C bus carries the SHT3x temp/humidity sensor (0x44) and the DS3231 RTC
// (0x68) — both confirmed by an I2C scan. They feed the two local screens, which
// show device-intrinsic data no server could provide (the same reason the offline
// glyph is baked in).
static const uint8_t I2C_SDA = 21;
static const uint8_t I2C_SCL = 22;
RTC_DS3231 rtc;
Adafruit_SHT31 sht;
// The clock's timezone and NTP servers live in config.h (TIMEZONE, NTP_SERVER_*).

// ---- Timing / limits -------------------------------------------------------
static const uint32_t POLL_INTERVAL_MS = 60000;  // how often we fetch the payload
static const uint8_t MAX_SCREENS = 8;             // caps memory; server sends few
static const uint16_t DEFAULT_ROTATE_S = 8;
static const uint32_t DEFAULT_STALE_S = 1800;  // 30 min, until the server says otherwise

static const uint8_t LOCAL_SCREENS = 2;                     // clock + temp/humidity, always shown
static const uint32_t CLOCK_SYNC_MS = 12UL * 3600 * 1000;  // re-sync RTC from NTP twice a day
static const uint16_t TEMPHUM_SWAP_MS = 2000;              // temp/humidity screen alternates this often

// ---- Screen model ----------------------------------------------------------
struct Screen {
  char text[16];
  uint16_t textColor;
  uint16_t icon[64];  // 8x8, RGB565, row-major
};

static Screen screens[MAX_SCREENS];
static uint8_t screenCount = 0;
static uint8_t currentScreen = 0;
static uint16_t rotateSeconds = DEFAULT_ROTATE_S;
static uint32_t staleAfterMs = DEFAULT_STALE_S * 1000UL;

static uint32_t lastPoll = 0;
static uint32_t lastRotate = 0;
static uint32_t lastSuccess = 0;  // millis() of the last good fetch
static bool haveData = false;

static bool autoRotate = true;    // a single middle click toggles this
static bool displayOn = true;     // a double middle click drops to standby
static bool needsRedraw = true;   // set by rotation or a button; drawn once per tick

static bool btnState[BTN_COUNT] = {false, false, false};   // debounced pressed state
static uint32_t btnChange[BTN_COUNT] = {0, 0, 0};          // millis() of last accepted edge
static uint8_t midClicks = 0;         // middle clicks counted inside the double-click window
static uint32_t midLastRelease = 0;   // millis() of the last middle release
static bool midWaking = false;        // the press that woke from standby isn't a click

static bool clockValid = false;       // RTC holds a trusted time (from NTP or kept by its battery)
static bool clockSynced = false;      // NTP has set the RTC at least once this boot
static uint32_t lastClockSync = 0;    // millis() of the last NTP sync
static uint32_t lastLocalTick = 0;    // paces the in-place refresh of the active local screen
static bool tempHumPhase = false;     // false = temperature, true = humidity

// Glyphs baked into the firmware. The alert is drawn when we're offline (no
// payload to pull an icon from); the thermometer and droplet mark the local
// sensor screen. Each is an 8x8 bitmap, MSB = leftmost column.
static const uint8_t ALERT_GLYPH[8] = {
    0b00011000, 0b00011000, 0b00111100, 0b00100100,
    0b01100110, 0b01111110, 0b01100110, 0b11111111,
};
static const uint8_t THERMO_GLYPH[8] = {
    0b00011000, 0b00011000, 0b00011000, 0b00011000,
    0b00111100, 0b01111110, 0b01111110, 0b00111100,
};
static const uint8_t DROP_GLYPH[8] = {
    0b00011000, 0b00011000, 0b00111100, 0b00111100,
    0b01111110, 0b01111110, 0b01111110, 0b00111100,
};

// ---- Color helpers ---------------------------------------------------------
static uint8_t hexPair(const char *s) {
  auto nib = [](char c) -> uint8_t {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  return (nib(s[0]) << 4) | nib(s[1]);
}

// "#rrggbb" or "rrggbb" → RGB565
static uint16_t parseColor(const char *hex) {
  if (hex[0] == '#') hex++;
  uint8_t r = hexPair(hex), g = hexPair(hex + 2), b = hexPair(hex + 4);
  return matrix.Color(r, g, b);
}

// 384-char hex string (64 pixels × rrggbb) → icon[64] in RGB565
static void parseIcon(const char *hex, uint16_t out[64]) {
  size_t len = strlen(hex);
  for (uint8_t i = 0; i < 64; i++) {
    size_t off = (size_t)i * 6;
    if (off + 6 > len) {
      out[i] = 0;
      continue;
    }
    out[i] = matrix.Color(hexPair(hex + off), hexPair(hex + off + 2), hexPair(hex + off + 4));
  }
}

// ---- WiFi ------------------------------------------------------------------
// Join the first known network in range. Blocks up to a few seconds per attempt;
// on failure the caller retries on the next loop tick.
static bool connectWifi() {
  int found = WiFi.scanNetworks();
  for (auto &net : WIFI_NETWORKS) {
    for (int i = 0; i < found; i++) {
      if (WiFi.SSID(i) != net.ssid) continue;
      WiFi.begin(net.ssid, net.password);
      for (int t = 0; t < 20 && WiFi.status() != WL_CONNECTED; t++) delay(250);
      if (WiFi.status() == WL_CONNECTED) {
        WiFi.scanDelete();
        return true;
      }
    }
  }
  WiFi.scanDelete();
  return false;
}

// ---- Clock -----------------------------------------------------------------
// Pull the time over NTP (WiFi must be up) and write it into the DS3231, so the
// clock stays accurate even while offline. Called opportunistically after a poll.
static void syncClock() {
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);
  struct tm t;
  if (!getLocalTime(&t, 5000)) {
    Serial.println("[atalaia] NTP: no answer");
    return;  // retry next window
  }
  rtc.adjust(DateTime(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec));
  clockValid = true;
  clockSynced = true;
  lastClockSync = millis();
  Serial.printf("[atalaia] NTP ok: %04d-%02d-%02d %02d:%02d\n", t.tm_year + 1900, t.tm_mon + 1,
                t.tm_mday, t.tm_hour, t.tm_min);
}

// ---- Fetch -----------------------------------------------------------------
static bool fetchScreens() {
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) return false;

  WiFiClientSecure client;
  if (strlen(API_ROOT_CA) > 0) {
    client.setCACert(API_ROOT_CA);  // validated TLS
  } else {
    client.setInsecure();  // skips cert check — fine on trusted WiFi, risky otherwise
  }

  HTTPClient https;
  https.setConnectTimeout(8000);
  https.setTimeout(8000);
  if (!https.begin(client, API_URL)) return false;
  https.addHeader("x-device-token", API_TOKEN);

  int code = https.GET();
  if (code != 200) {
    https.end();
    return false;
  }

  // Stream the parse so we never hold the whole body plus the DOM in RAM at once.
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, https.getStream());
  https.end();
  if (err) return false;

  rotateSeconds = doc["rotateSeconds"] | DEFAULT_ROTATE_S;
  uint32_t staleS = doc["staleAfter"] | DEFAULT_STALE_S;
  staleAfterMs = staleS * 1000UL;

  JsonArray arr = doc["screens"].as<JsonArray>();
  uint8_t n = 0;
  for (JsonObject s : arr) {
    if (n >= MAX_SCREENS) break;
    strlcpy(screens[n].text, s["text"] | "", sizeof(screens[n].text));
    screens[n].textColor = parseColor(s["color"] | "#ffffff");
    parseIcon(s["icon"] | "", screens[n].icon);
    n++;
  }
  screenCount = n;
  if (currentScreen >= screenCount) currentScreen = 0;
  return screenCount > 0;
}

// ---- Render ----------------------------------------------------------------
// Blit an 8x8 baked glyph into the left cell in one color.
static void drawGlyph(const uint8_t glyph[8], uint16_t color) {
  for (uint8_t y = 0; y < 8; y++)
    for (uint8_t x = 0; x < 8; x++)
      if (glyph[y] & (0x80 >> x)) matrix.drawPixel(x, y, color);
}

static void drawScreen(const Screen &s) {
  matrix.setFont();  // default 5x7 font (a local screen may have left TomThumb set)
  matrix.fillScreen(0);
  for (uint8_t y = 0; y < 8; y++)
    for (uint8_t x = 0; x < 8; x++) matrix.drawPixel(x, y, s.icon[y * 8 + x]);

  // Right-align the value inside the 24px region to the right of the icon.
  int16_t bx, by;
  uint16_t bw, bh;
  matrix.setTextWrap(false);
  matrix.getTextBounds(s.text, 0, 0, &bx, &by, &bw, &bh);
  int16_t x = 32 - (int16_t)bw;
  if (x < 9) x = 9;  // long values start flush and may clip; scrolling is a future step
  matrix.setCursor(x, 1);
  matrix.setTextColor(s.textColor);
  matrix.print(s.text);
  matrix.show();
}

static void drawAlert() {
  const uint16_t amber = matrix.Color(255, 120, 0);
  matrix.setFont();
  matrix.fillScreen(0);
  drawGlyph(ALERT_GLYPH, amber);
  matrix.setTextWrap(false);
  matrix.setCursor(12, 1);
  matrix.setTextColor(amber);
  matrix.print("OFF");
  matrix.show();
}

// Local screen: a little calendar with the day-of-month on the left, HH:MM (24h,
// small font) on the right. Reads the DS3231; shows dashes until the clock is set.
static void drawClock() {
  matrix.fillScreen(0);
  DateTime now = rtc.now();

  matrix.fillRect(0, 0, 8, 8, matrix.Color(35, 35, 40));  // calendar body
  matrix.fillRect(0, 0, 8, 2, matrix.Color(200, 40, 40));  // binding strip on top
  matrix.setFont(&TomThumb);
  matrix.setTextWrap(false);
  matrix.setTextColor(matrix.Color(255, 255, 255));
  matrix.setCursor(1, 7);  // TomThumb baseline sits at the cursor y
  if (clockValid) {
    char day[3];
    snprintf(day, sizeof(day), "%02d", now.day());
    matrix.print(day);
  } else {
    matrix.print("--");
  }

  matrix.setTextColor(matrix.Color(0, 180, 255));
  matrix.setCursor(10, 6);
  if (clockValid) {
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", now.hour(), now.minute());
    matrix.print(hhmm);
  } else {
    matrix.print("--:--");
  }

  matrix.setFont();  // restore default for the other screens
  matrix.show();
}

// Local screen: alternates between temperature and humidity from the SHT3x, each
// with its glyph on the left and the value in the default font on the right.
static void drawTempHum(bool humidity) {
  matrix.setFont();
  matrix.fillScreen(0);
  matrix.setTextWrap(false);
  char buf[8];
  if (!humidity) {
    float t = sht.readTemperature();
    drawGlyph(THERMO_GLYPH, matrix.Color(255, 80, 0));
    if (isnan(t)) strlcpy(buf, "--", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%.0f\xF7", t);  // 0xF7 = degree ring in the default font
    matrix.setTextColor(matrix.Color(255, 160, 40));
  } else {
    float h = sht.readHumidity();
    drawGlyph(DROP_GLYPH, matrix.Color(0, 140, 255));
    if (isnan(h)) strlcpy(buf, "--", sizeof(buf));
    else snprintf(buf, sizeof(buf), "%.0f%%", h);
    matrix.setTextColor(matrix.Color(80, 180, 255));
  }
  matrix.setCursor(11, 1);
  matrix.print(buf);
  matrix.show();
}

static bool isStale() {
  return !haveData || (millis() - lastSuccess) > staleAfterMs;
}

// The rotation is [primary slots] + [clock] + [temp/humidity]. The primary slots
// are the server screens when fresh, or a single offline glyph when stale — so we
// never show a stale server number, but the local screens stay visible either way.
static uint8_t primarySlots() { return isStale() ? 1 : screenCount; }
static uint8_t totalSlots() { return primarySlots() + LOCAL_SCREENS; }

static void drawCurrent() {
  uint8_t primary = primarySlots();
  if (currentScreen < primary) {
    if (isStale()) drawAlert();
    else drawScreen(screens[currentScreen]);
  } else if (currentScreen == primary) {
    drawClock();
  } else {
    drawTempHum(tempHumPhase);
  }
}

// ---- Buttons ---------------------------------------------------------------
// Step the current screen by delta (±1), wrapping, and reset the rotation clock
// so an auto-advance doesn't fire on top of a manual move.
static void stepScreen(int8_t delta) {
  uint8_t total = totalSlots();
  currentScreen = (currentScreen + total + delta) % total;
  lastRotate = millis();
  lastLocalTick = millis();
  needsRedraw = true;
}

// Turn the panel on (redraw the current screen) or off (blank it, stop drawing).
static void setDisplayOn(bool on) {
  displayOn = on;
  if (on) {
    matrix.setBrightness(BRIGHTNESS);
    needsRedraw = true;
  } else {
    matrix.fillScreen(0);
    matrix.show();
  }
}

// Poll the buttons and act on their edges. Simple debounce: ignore any change
// that lands within DEBOUNCE_MS of the last accepted one for that button.
//
// Middle button: while on, a single click toggles rotation and a double click
// drops to standby; while off, any press wakes it. The single-click action waits
// out the double-click window so a double click never also toggles rotation.
static void handleButtons() {
  uint32_t now = millis();

  bool pressedEdge[BTN_COUNT] = {false, false, false};
  bool releasedEdge[BTN_COUNT] = {false, false, false};
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    bool raw = digitalRead(BTN_PINS[i]) == LOW;  // active-low
    if (raw == btnState[i] || now - btnChange[i] < DEBOUNCE_MS) continue;
    btnState[i] = raw;
    btnChange[i] = now;
    (raw ? pressedEdge : releasedEdge)[i] = true;
  }

  if (pressedEdge[BTN_MID] && !displayOn) {  // wake from standby
    setDisplayOn(true);
    midClicks = 0;
    midWaking = true;
  }
  if (releasedEdge[BTN_MID]) {
    if (midWaking) {
      midWaking = false;  // the wake press doesn't count as a click
    } else if (displayOn) {
      midLastRelease = now;
      if (++midClicks >= 2) {  // double click → standby
        midClicks = 0;
        setDisplayOn(false);
      }
    }
  }
  if (midClicks == 1 && now - midLastRelease > DOUBLE_CLICK_MS) {  // lone click → toggle
    midClicks = 0;
    autoRotate = !autoRotate;
    lastRotate = now;
  }

  if (displayOn && pressedEdge[BTN_LEFT]) stepScreen(-1);
  if (displayOn && pressedEdge[BTN_RIGHT]) stepScreen(+1);
}

// ---- Lifecycle -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // silence the piezo before anything else
  Serial.println("\n[atalaia] boot");

  for (uint8_t i = 0; i < BTN_COUNT; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
  matrix.begin();
  matrix.setBrightness(BRIGHTNESS);
  matrix.setTextColor(matrix.Color(255, 255, 255));
  matrix.fillScreen(0);
  matrix.show();

  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.printf("[atalaia] rtc.begin=%d lostPower=%d  sht.begin=%d\n", rtc.begin(),
                rtc.lostPower(), sht.begin(0x44));
  if (!rtc.lostPower()) clockValid = true;  // the RTC battery kept a real time

  if (fetchScreens()) {
    haveData = true;
    lastSuccess = millis();
  }
  Serial.printf("[atalaia] boot fetch: %u screens, wifi=%d\n", screenCount,
                WiFi.status() == WL_CONNECTED);
  if (WiFi.status() == WL_CONNECTED) syncClock();  // set the RTC from NTP on boot
  lastPoll = millis();
}

void loop() {
  uint32_t now = millis();

  handleButtons();

  if (!displayOn) {  // standby: panel dark, nothing to poll or draw
    delay(20);
    return;
  }

  if (now - lastPoll >= POLL_INTERVAL_MS) {
    lastPoll = now;
    if (fetchScreens()) {
      haveData = true;
      lastSuccess = now;
      needsRedraw = true;
    }
    if (WiFi.status() == WL_CONNECTED && (!clockSynced || now - lastClockSync >= CLOCK_SYNC_MS))
      syncClock();
    DateTime t = rtc.now();
    Serial.printf("[atalaia] poll: screens=%u stale=%d clock=%02d:%02d temp=%.1f hum=%.1f\n",
                  screenCount, isStale(), t.hour(), t.minute(), sht.readTemperature(),
                  sht.readHumidity());
  }

  uint8_t total = totalSlots();
  if (currentScreen >= total) {  // rotation shrank (e.g. went stale); wrap back in
    currentScreen = 0;
    needsRedraw = true;
  }

  // Auto-advance unless the user paused rotation with a single middle click.
  if (autoRotate && now - lastRotate >= (uint32_t)rotateSeconds * 1000UL) {
    currentScreen = (currentScreen + 1) % total;
    lastRotate = now;
    lastLocalTick = now;
    needsRedraw = true;
  }

  // Local screens refresh in place while shown: the clock ticks every second, the
  // temp/humidity screen alternates between its two readings.
  uint8_t primary = primarySlots();
  if (currentScreen == primary && now - lastLocalTick >= 1000) {
    lastLocalTick = now;
    needsRedraw = true;
  } else if (currentScreen == primary + 1 && now - lastLocalTick >= TEMPHUM_SWAP_MS) {
    lastLocalTick = now;
    tempHumPhase = !tempHumPhase;
    needsRedraw = true;
  }

  if (needsRedraw) {
    drawCurrent();
    needsRedraw = false;
  }

  delay(20);
}
