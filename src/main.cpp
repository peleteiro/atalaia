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
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

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

// ---- Timing / limits -------------------------------------------------------
static const uint32_t POLL_INTERVAL_MS = 60000;  // how often we fetch the payload
static const uint8_t MAX_SCREENS = 8;             // caps memory; server sends few
static const uint16_t DEFAULT_ROTATE_S = 8;
static const uint32_t DEFAULT_STALE_S = 1800;  // 30 min, until the server says otherwise

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

// Amber warning triangle — the only glyph baked into the firmware, because when
// we're offline there's no payload to pull an icon from.
static const uint8_t ALERT_GLYPH[8] = {
    0b00011000, 0b00011000, 0b00111100, 0b00100100,
    0b01100110, 0b01111110, 0b01100110, 0b11111111,
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
static void drawScreen(const Screen &s) {
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
  matrix.fillScreen(0);
  for (uint8_t y = 0; y < 8; y++)
    for (uint8_t x = 0; x < 8; x++)
      if (ALERT_GLYPH[y] & (0x80 >> x)) matrix.drawPixel(x, y, amber);
  matrix.setTextWrap(false);
  matrix.setCursor(12, 1);
  matrix.setTextColor(amber);
  matrix.print("OFF");
  matrix.show();
}

static bool isStale() {
  return !haveData || (millis() - lastSuccess) > staleAfterMs;
}

// ---- Buttons ---------------------------------------------------------------
// Step the current screen by delta (±1), wrapping, and reset the rotation clock
// so an auto-advance doesn't fire on top of a manual move.
static void stepScreen(int8_t delta) {
  if (screenCount == 0) return;
  currentScreen = (currentScreen + screenCount + delta) % screenCount;
  lastRotate = millis();
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
  for (uint8_t i = 0; i < BTN_COUNT; i++) pinMode(BTN_PINS[i], INPUT_PULLUP);
  matrix.begin();
  matrix.setBrightness(BRIGHTNESS);
  matrix.setTextColor(matrix.Color(255, 255, 255));
  matrix.fillScreen(0);
  matrix.show();

  if (fetchScreens()) {
    haveData = true;
    lastSuccess = millis();
  }
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
  }

  if (isStale()) {
    drawAlert();
    delay(20);  // short, so a button press still registers promptly
    return;
  }

  // Auto-advance unless the user paused rotation with the middle button.
  if (autoRotate && now - lastRotate >= (uint32_t)rotateSeconds * 1000UL) {
    currentScreen = (currentScreen + 1) % screenCount;
    lastRotate = now;
    needsRedraw = true;
  }

  if (needsRedraw) {
    drawScreen(screens[currentScreen]);
    needsRedraw = false;
  }

  delay(20);
}
