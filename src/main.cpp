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
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <time.h>

#include "config.h"
#include "secrets.h"

// ---- Hardware (Ulanzi TC001) -----------------------------------------------
// The matrix data pin and orientation flags below match the TC001, confirmed on
// hardware: origin top-left, and the rows are wired serpentine (ZIGZAG) — odd
// rows run right-to-left. With PROGRESSIVE instead, a compact drawing splits into
// a left half and a mirrored right half.
static const uint8_t MATRIX_PIN = 32;
static const uint8_t BRIGHTNESS = 40;  // 0-255; the panel is bright up close.

Adafruit_NeoMatrix matrix(32, 8, MATRIX_PIN,
                          NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG,
                          NEO_GRB + NEO_KHZ800);

// The three top-edge buttons, active-low (internal pull-up, pressed = LOW). Pins
// match the TC001; verify on hardware if a press does nothing on first flash.
// Left/right step through screens; the middle button pauses/resumes rotation.
enum Btn { BTN_LEFT, BTN_MID, BTN_RIGHT, BTN_COUNT };
static const uint8_t BTN_PINS[BTN_COUNT] = {26, 27, 14};
static const uint16_t DEBOUNCE_MS = 30;
static const uint16_t DOUBLE_CLICK_MS = 500;   // window to catch a second middle click
static const uint16_t HOLD_OFF_MS = 5000;      // hold the middle button this long → deep sleep

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
static const uint8_t MAX_SCREENS = 16;            // caps memory; server sends few
static const uint16_t DEFAULT_ROTATE_S = 8;
static const uint32_t DEFAULT_STALE_S = 1800;  // 30 min, until the server says otherwise

static const uint8_t LOCAL_SCREENS = 2;                     // clock + temp/humidity, always shown
static const uint32_t CLOCK_SYNC_MS = 12UL * 3600 * 1000;  // re-sync RTC from NTP twice a day
static const uint16_t TEMPHUM_REFRESH_MS = 2000;          // how often the sensor screen re-reads

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
static uint32_t midPressStart = 0;    // millis() the middle button went down (for hold-to-off)
static bool midHeldFired = false;     // the current hold already triggered power-off

static bool clockValid = false;       // RTC holds a trusted time (from NTP or kept by its battery)
static bool clockSynced = false;      // NTP has set the RTC at least once this boot
static uint32_t lastClockSync = 0;    // millis() of the last NTP sync
static uint32_t lastLocalTick = 0;    // paces the in-place refresh of the active local screen

// Glyphs baked into the firmware. The alert is drawn when we're offline (no
// payload to pull an icon from). The thermometer marks the sensor screen and is
// two-toned: a white outline (THERMO_WHITE) over a red mercury column and bulb
// (THERMO_RED). Each is an 8x8 bitmap, MSB = leftmost column.
static const uint8_t ALERT_GLYPH[8] = {
    0b00011000, 0b00011000, 0b00111100, 0b00100100,
    0b01100110, 0b01111110, 0b01100110, 0b11111111,
};
static const uint8_t THERMO_WHITE[8] = {
    0b00111100, 0b00100100, 0b00100100, 0b00100100,
    0b01100110, 0b01000010, 0b01000010, 0b00111100,
};
static const uint8_t THERMO_RED[8] = {
    0b00000000, 0b00000000, 0b00000000, 0b00011000,
    0b00011000, 0b00111100, 0b00111100, 0b00000000,
};

// 3x5 pixel digits 0-9 for the clock screen. Each row uses the low 3 bits, bit 2
// = leftmost column. Hand-drawn so the clock reads cleanly at this tiny size.
static const uint8_t DIGITS_3x5[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111},  // 0
    {0b010, 0b110, 0b010, 0b010, 0b111},  // 1
    {0b111, 0b001, 0b111, 0b100, 0b111},  // 2
    {0b111, 0b001, 0b111, 0b001, 0b111},  // 3
    {0b101, 0b101, 0b111, 0b001, 0b001},  // 4
    {0b111, 0b100, 0b111, 0b001, 0b111},  // 5
    {0b111, 0b100, 0b111, 0b101, 0b111},  // 6
    {0b111, 0b001, 0b010, 0b010, 0b010},  // 7
    {0b111, 0b101, 0b111, 0b101, 0b111},  // 8
    {0b111, 0b101, 0b111, 0b001, 0b111},  // 9
};
// A 3x5 percent sign for the humidity screen, same grid as the digits.
static const uint8_t PERCENT_3x5[5] = {0b101, 0b001, 0b010, 0b100, 0b101};

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
  matrix.setFont(&TomThumb);  // small AWTRIX-style font, matching the local screens
  matrix.fillScreen(0);
  for (uint8_t y = 0; y < 8; y++)  // the icon is always 8x8; anything larger is cropped
    for (uint8_t x = 0; x < 8; x++) matrix.drawPixel(x, y, s.icon[y * 8 + x]);

  // Center the value in the 24px region to the right of the icon, both axes.
  matrix.setTextWrap(false);
  int16_t bx, by;
  uint16_t bw, bh;
  matrix.getTextBounds(s.text, 0, 0, &bx, &by, &bw, &bh);
  int16_t tx = 8 + (24 - (int16_t)bw) / 2;  // region is cols 8..31
  int16_t ty = (8 - (int16_t)bh) / 2 + 1;   // one pixel below center reads better here
  if (tx < 8) tx = 8;  // oversized text starts flush and crops on the right
  matrix.setCursor(tx - bx, ty - by);  // place the text box's top-left at (tx, ty)
  matrix.setTextColor(s.textColor);
  matrix.print(s.text);
  matrix.show();
}

static void drawAlert() {
  const uint16_t amber = matrix.Color(255, 120, 0);
  matrix.setFont(&TomThumb);
  matrix.fillScreen(0);
  drawGlyph(ALERT_GLYPH, amber);
  matrix.setTextWrap(false);
  matrix.setCursor(11, 6);
  matrix.setTextColor(amber);
  matrix.print("OFF");
  matrix.show();
}

// Draw a 3x5 cell (digit, percent, …) with its top-left at (x, y).
static void draw3x5(const uint8_t rows[5], int16_t x, int16_t y, uint16_t color) {
  for (uint8_t r = 0; r < 5; r++)
    for (uint8_t c = 0; c < 3; c++)
      if (rows[r] & (0b100 >> c)) matrix.drawPixel(x + c, y + r, color);
}

static void drawDigit(uint8_t d, int16_t x, int16_t y, uint16_t color) {
  if (d <= 9) draw3x5(DIGITS_3x5[d], x, y, color);
}

// A 3px dash in a digit slot — shown for each digit until the clock is set.
static void drawDash(int16_t x, int16_t y, uint16_t color) {
  for (uint8_t col = 0; col < 3; col++) matrix.drawPixel(x + col, y + 2, color);
}

// Local screen, modeled on the stock Ulanzi clock: a 9px-wide white calendar page
// with a red header and the day-of-month cut out of the white (drawn in black),
// then HH:MM (24h) in white on the right. Reads the DS3231; shows dashes until the
// clock is set. No seconds bar.
static void drawClock() {
  matrix.fillScreen(0);
  const uint16_t white = matrix.Color(255, 255, 255);

  matrix.fillRect(0, 0, 9, 8, white);                      // white calendar page (9px)
  matrix.fillRect(0, 0, 9, 2, matrix.Color(200, 30, 30));  // red header strip
  if (clockValid) {
    uint8_t day = rtc.now().day();
    drawDigit(day / 10, 1, 2, 0);  // 0 = off: cut the day out of the white page
    drawDigit(day % 10, 5, 2, 0);
  } else {
    drawDash(1, 2, 0);
    drawDash(5, 2, 0);
  }

  const int16_t x = 12, y = 2;  // time block, right of the calendar
  if (clockValid) {
    DateTime now = rtc.now();
    uint8_t hh = now.hour(), mm = now.minute();
    drawDigit(hh / 10, x, y, white);
    drawDigit(hh % 10, x + 4, y, white);
    // Colon: on for most of the second, then a quick fade-out and fade-in.
    uint32_t ph = millis() % 1000;
    uint8_t cb = ph < 750 ? 255
                 : ph < 875 ? (uint8_t)(255 - (ph - 750) * 255 / 125)   // fade out
                            : (uint8_t)((ph - 875) * 255 / 125);         // fade in
    uint16_t colon = matrix.Color(cb, cb, cb);
    matrix.drawPixel(x + 8, y + 1, colon);
    matrix.drawPixel(x + 8, y + 3, colon);
    drawDigit(mm / 10, x + 10, y, white);
    drawDigit(mm % 10, x + 14, y, white);
  } else {
    drawDash(x, y, white);
    drawDash(x + 4, y, white);
    drawDash(x + 10, y, white);
    drawDash(x + 14, y, white);
  }

  matrix.show();
}

// Draw a two-digit value (0-99) in the 3x5 digits at (x, y), or dashes if NaN.
static void drawValue(float v, int16_t x, int16_t y, uint16_t color) {
  if (isnan(v)) {
    drawDash(x, y, color);
    drawDash(x + 4, y, color);
    return;
  }
  int n = constrain((int)lroundf(v), 0, 99);
  drawDigit(n / 10, x, y, color);
  drawDigit(n % 10, x + 4, y, color);
}

// Local screen: temperature and humidity together on one panel — a thermometer
// icon, then NN° (warm) and NN% (cool) side by side, in the clock's 3x5 digits.
static void drawTempHum() {
  matrix.fillScreen(0);
  const uint16_t white = matrix.Color(255, 255, 255);
  const uint16_t cool = matrix.Color(100, 180, 255);
  const int16_t y = 2;
  drawGlyph(THERMO_WHITE, white);              // white thermometer outline …
  drawGlyph(THERMO_RED, matrix.Color(255, 30, 30));  // … over the red mercury

  drawValue(sht.readTemperature(), 9, y, white);  // NN° at cols 9..18
  matrix.drawPixel(17, y, white);                 // degree ring (2x2, sits high)
  matrix.drawPixel(18, y, white);
  matrix.drawPixel(17, y + 1, white);
  matrix.drawPixel(18, y + 1, white);

  drawValue(sht.readHumidity(), 20, y, cool);     // NN% at cols 20..30
  draw3x5(PERCENT_3x5, 28, y, cool);

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
    drawTempHum();
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
  Serial.printf("[atalaia] display %s\n", on ? "ON" : "STANDBY");
  if (on) {
    matrix.setBrightness(BRIGHTNESS);
    needsRedraw = true;
  } else {
    matrix.fillScreen(0);
    matrix.show();
  }
}

// Power the device down into deep sleep to save battery. The middle button wakes
// it (which reboots into setup). Does not return.
static void powerOff() {
  Serial.println("[atalaia] power off — deep sleep");
  matrix.fillScreen(0);
  matrix.show();

  // Wait for release, or the still-pressed button would wake us instantly.
  while (digitalRead(BTN_PINS[BTN_MID]) == LOW) delay(10);
  delay(50);

  // Hold the buzzer pin low through sleep — floating, it squeals.
  digitalWrite(BUZZER_PIN, LOW);
  gpio_hold_en((gpio_num_t)BUZZER_PIN);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PINS[BTN_MID], 0);  // wake on press (low)
  esp_deep_sleep_start();
}

// Poll the buttons and act on their edges. Simple debounce: ignore any change
// that lands within DEBOUNCE_MS of the last accepted one for that button.
//
// Middle button: a single click toggles rotation, a double click drops to
// standby, and a ~5s hold powers off (deep sleep). While off/standby, any press
// wakes it. The single-click action waits out the double-click window so a double
// click never also toggles rotation.
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

  if (pressedEdge[BTN_MID]) {
    if (!displayOn) {  // wake from standby
      setDisplayOn(true);
      midClicks = 0;
      midWaking = true;
    } else {  // start timing the hold
      midPressStart = now;
      midHeldFired = false;
    }
  }
  // Held ~5s → deep sleep. Guarded so the wake-from-standby press can't trigger it.
  if (btnState[BTN_MID] && displayOn && !midWaking && !midHeldFired &&
      now - midPressStart >= HOLD_OFF_MS) {
    midHeldFired = true;
    powerOff();  // does not return
  }
  if (releasedEdge[BTN_MID]) {
    if (midWaking) {
      midWaking = false;  // the wake press doesn't count as a click
    } else if (midHeldFired) {
      midHeldFired = false;  // the long hold already acted; not a click
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
    Serial.printf("[atalaia] autoRotate %s\n", autoRotate ? "ON" : "OFF");
  }

  if (displayOn && pressedEdge[BTN_LEFT]) stepScreen(-1);
  if (displayOn && pressedEdge[BTN_RIGHT]) stepScreen(+1);
}

// ---- Lifecycle -------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  gpio_hold_dis((gpio_num_t)BUZZER_PIN);  // release the hold set before a prior deep sleep
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);  // silence the piezo before anything else
  Serial.printf("\n[atalaia] boot (wake cause %d)\n", (int)esp_sleep_get_wakeup_cause());

  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    btnState[i] = digitalRead(BTN_PINS[i]) == LOW;  // held at boot (wake press) → no phantom edge
  }
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
  if (currentScreen == primary && now - lastLocalTick >= 60) {  // fast refresh for the colon fade
    lastLocalTick = now;
    needsRedraw = true;
  } else if (currentScreen == primary + 1 && now - lastLocalTick >= TEMPHUM_REFRESH_MS) {
    lastLocalTick = now;  // refresh the sensor readings in place
    needsRedraw = true;
  }

  if (needsRedraw) {
    drawCurrent();
    needsRedraw = false;
  }

  delay(20);
}
