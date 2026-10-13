/*
  MG996R Servo Payload Release - Teensy 4.1 (Ethernet web control)
  -----------------------------------------------------------------
  Drives an MG996/MG996R servo-powered payload release (drop) mechanism and
  serves a control page over the Teensy 4.1's built-in Ethernet.

  Ways to control it:
    1. Web GUI   - browse to http://192.168.2.50 (or http://payload.local)
    2. USB Serial commands (115200 baud, newline terminated) - type "help"
    3. Optional push button on BUTTON_PIN (short press = drop, hold 2 s = arm/disarm)

  Safety:
    - The unit boots DISARMED. Every drop (web, serial, button) needs it armed.
    - Web actions use POST, so a browser prefetch/link preview can't fire a drop.
    - Set WEB_KEY to require a key for web actions. Keep it on your LAN - do
      NOT port-forward this to the internet (use a VPN if you need remote access).

  Hardware:
    Teensy 4.1 + Ethernet kit (the MagJack/RJ45 adapter soldered to the 6-pin
    Ethernet header) plugged into your router/switch.

  Wiring (see README.md):
    Servo signal (orange) -> Teensy pin 9
    Servo +V (red)        -> external 5-6 V supply, 3 A or more (NOT Teensy 5V/3.3V)
    Servo GND (brown)     -> supply GND AND Teensy GND (common ground required)
    Button (optional)     -> pin 2 to GND (internal pull-up used)

  Calibration values are stored in EEPROM, so set them once and press "Save".
*/

#include <Servo.h>
#include <EEPROM.h>
#include <QNEthernet.h>
#include "WebPage.h"

using namespace qindesign::network;

// ---------------- User configuration ----------------
const uint8_t SERVO_PIN  = 9;
const uint8_t BUTTON_PIN = 2;
const uint8_t LED_PIN    = LED_BUILTIN;   // pin 13 on Teensy 4.1

#define USE_BUTTON 1   // set to 0 if no button is fitted

// Network. A fixed address is used by default; set USE_DHCP to 1 to get an
// address from your router instead (the STATIC_* values are then ignored).
#define USE_DHCP 0
const IPAddress STATIC_IP(192, 168, 2, 50);       // -> http://192.168.2.50
const IPAddress STATIC_MASK(255, 255, 255, 0);
const IPAddress STATIC_GATEWAY(192, 168, 2, 1);
const char *HOSTNAME = "payload";          // -> http://payload.local
const uint16_t HTTP_PORT = 80;

// Optional access key for web actions. "" = no key needed.
// If set, enter the same key in the web page's Settings box.
const char *WEB_KEY = "";

// Default positions in microseconds. MG996R covers ~180 deg over 500-2500 us.
// Use the jog slider (web) or "jog" command to find values for your mechanism.
const uint16_t DEFAULT_CLOSED_US     = 1350;  // hook/latch closed (holding payload)
const uint16_t DEFAULT_OPEN_US       = 2000;  // hook/latch open (payload released)
const uint32_t DEFAULT_AUTOCLOSE_MS  = 3000;  // 0 = stay open after a drop

const uint16_t SERVO_MIN_US = 500;            // hard limits for any command
const uint16_t SERVO_MAX_US = 2500;
const float    CLOSE_SPEED_US_PER_MS = 1.0f;  // slow close so it doesn't snap/jam

const uint32_t BUTTON_DEBOUNCE_MS = 30;
const uint32_t BUTTON_HOLD_MS     = 2000;
const uint32_t HTTP_TIMEOUT_MS    = 1000;     // max time to wait for a request

// ---------------- Persistent settings ----------------
struct Settings {
  uint32_t magic;
  uint16_t closedUs;
  uint16_t openUs;
  uint32_t autoCloseMs;
};
const uint32_t SETTINGS_MAGIC = 0x4D473936;   // "MG96"
const int      SETTINGS_ADDR  = 0;
Settings cfg;

// ---------------- State ----------------
Servo servo;
bool armed = false;
bool isOpen = false;
float currentUs = 0;
uint16_t targetUs = 0;
bool slewing = false;
uint32_t lastSlewMs = 0;
uint32_t openedAtMs = 0;
bool autoClosePending = false;   // only drops auto-close; manual "open" stays open
uint32_t dropCount = 0;

// Network
EthernetServer server{HTTP_PORT};
bool networkReported = false;

// Button
bool buttonStable = HIGH;
bool buttonLastRead = HIGH;
uint32_t buttonChangedMs = 0;
uint32_t buttonPressedMs = 0;
bool buttonHoldHandled = false;

// Serial line buffer
char lineBuf[64];
uint8_t lineLen = 0;

// ---------------- Helpers ----------------
uint16_t clampUs(long us) {
  if (us < SERVO_MIN_US) return SERVO_MIN_US;
  if (us > SERVO_MAX_US) return SERVO_MAX_US;
  return (uint16_t)us;
}

void loadSettings() {
  EEPROM.get(SETTINGS_ADDR, cfg);
  if (cfg.magic != SETTINGS_MAGIC) {
    cfg.magic = SETTINGS_MAGIC;
    cfg.closedUs = DEFAULT_CLOSED_US;
    cfg.openUs = DEFAULT_OPEN_US;
    cfg.autoCloseMs = DEFAULT_AUTOCLOSE_MS;
  }
  cfg.closedUs = clampUs(cfg.closedUs);
  cfg.openUs = clampUs(cfg.openUs);
}

void saveSettings() {
  EEPROM.put(SETTINGS_ADDR, cfg);
  Serial.println(F("Settings saved to EEPROM."));
}

void restoreDefaults() {
  cfg.closedUs = DEFAULT_CLOSED_US;
  cfg.openUs = DEFAULT_OPEN_US;
  cfg.autoCloseMs = DEFAULT_AUTOCLOSE_MS;
}

// Jump straight to a position (used for opening - we want it fast).
void moveNow(uint16_t us) {
  us = clampUs(us);
  slewing = false;
  targetUs = us;
  currentUs = us;
  servo.writeMicroseconds(us);
}

// Move gradually to a position (used for closing).
void moveSlow(uint16_t us) {
  targetUs = clampUs(us);
  slewing = true;
  lastSlewMs = millis();
}

void updateSlew() {
  if (!slewing) return;
  uint32_t now = millis();
  float step = (now - lastSlewMs) * CLOSE_SPEED_US_PER_MS;
  lastSlewMs = now;
  if (step <= 0) return;

  if (currentUs < targetUs) {
    currentUs = min(currentUs + step, (float)targetUs);
  } else {
    currentUs = max(currentUs - step, (float)targetUs);
  }
  servo.writeMicroseconds((int)(currentUs + 0.5f));
  if ((uint16_t)(currentUs + 0.5f) == targetUs) slewing = false;
}

void openRelease(const char *source) {
  moveNow(cfg.openUs);
  isOpen = true;
  autoClosePending = false;
  openedAtMs = millis();
  Serial.print(F("RELEASE OPEN ("));
  Serial.print(source);
  Serial.println(F(")"));
}

void closeRelease() {
  moveSlow(cfg.closedUs);
  isOpen = false;
  autoClosePending = false;
  Serial.println(F("Release closing."));
}

// A "drop" is an open followed by auto-close (if enabled). Always needs arming.
bool drop(const char *source) {
  if (!armed) {
    Serial.println(F("Drop refused: DISARMED."));
    return false;
  }
  dropCount++;
  openRelease(source);
  autoClosePending = cfg.autoCloseMs > 0;
  return true;
}

void setArmed(bool a) {
  armed = a;
  Serial.println(armed ? F("ARMED - drops enabled.") : F("DISARMED."));
}

void printStatus() {
  Serial.println(F("---- Payload release status ----"));
  Serial.print(F("Armed:        ")); Serial.println(armed ? F("YES") : F("no"));
  Serial.print(F("State:        ")); Serial.println(isOpen ? F("OPEN") : F("closed"));
  Serial.print(F("Servo pos:    ")); Serial.print((int)(currentUs + 0.5f)); Serial.println(F(" us"));
  Serial.print(F("Closed pos:   ")); Serial.print(cfg.closedUs); Serial.println(F(" us"));
  Serial.print(F("Open pos:     ")); Serial.print(cfg.openUs); Serial.println(F(" us"));
  Serial.print(F("Auto-close:   "));
  if (cfg.autoCloseMs) { Serial.print(cfg.autoCloseMs); Serial.println(F(" ms")); }
  else Serial.println(F("off"));
  Serial.print(F("Drops:        ")); Serial.println(dropCount);
  Serial.print(F("Ethernet:     "));
  if (!Ethernet.linkState()) Serial.println(F("no cable/link"));
  else if (uint32_t(Ethernet.localIP()) == 0) Serial.println(F("link up, waiting for IP"));
  else { Serial.print(F("http://")); Serial.println(Ethernet.localIP()); }
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  arm | disarm          enable/disable drops"));
  Serial.println(F("  drop                  release payload (must be armed)"));
  Serial.println(F("  open | close          move release manually (no auto-close)"));
  Serial.println(F("  status                show current state and IP address"));
  Serial.println(F("  jog <us>              move servo to raw position (calibration)"));
  Serial.println(F("  set closed <us>       set closed position (500-2500)"));
  Serial.println(F("  set open <us>         set open position (500-2500)"));
  Serial.println(F("  set autoclose <ms>    auto-close delay after drop, 0 = off"));
  Serial.println(F("  save                  store settings in EEPROM"));
  Serial.println(F("  defaults              restore default settings (not saved)"));
}

void setClosed(long v) {
  cfg.closedUs = clampUs(v);
  if (!isOpen) moveSlow(cfg.closedUs);
}

void setOpen(long v) {
  cfg.openUs = clampUs(v);
  if (isOpen) moveNow(cfg.openUs);
}

void setAutoClose(long v) {
  cfg.autoCloseMs = v < 0 ? 0 : (uint32_t)v;
}

// ---------------- Serial command handling ----------------
void handleCommand(char *line) {
  // lower-case and trim
  for (char *p = line; *p; p++) *p = tolower(*p);
  while (*line == ' ') line++;
  if (*line == 0) return;

  char *cmd = strtok(line, " ");
  char *arg1 = strtok(NULL, " ");
  char *arg2 = strtok(NULL, " ");

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    printHelp();
  } else if (!strcmp(cmd, "arm")) {
    setArmed(true);
  } else if (!strcmp(cmd, "disarm")) {
    setArmed(false);
  } else if (!strcmp(cmd, "drop")) {
    drop("serial");
  } else if (!strcmp(cmd, "open")) {
    openRelease("manual");
  } else if (!strcmp(cmd, "close")) {
    closeRelease();
  } else if (!strcmp(cmd, "status")) {
    printStatus();
  } else if (!strcmp(cmd, "jog") && arg1) {
    uint16_t us = clampUs(atol(arg1));
    moveNow(us);
    Serial.print(F("Servo at ")); Serial.print(us); Serial.println(F(" us"));
  } else if (!strcmp(cmd, "set") && arg1 && arg2) {
    long v = atol(arg2);
    if (!strcmp(arg1, "closed")) {
      setClosed(v);
      Serial.print(F("Closed = ")); Serial.println(cfg.closedUs);
    } else if (!strcmp(arg1, "open")) {
      setOpen(v);
      Serial.print(F("Open = ")); Serial.println(cfg.openUs);
    } else if (!strcmp(arg1, "autoclose")) {
      setAutoClose(v);
      Serial.print(F("Auto-close = ")); Serial.println(cfg.autoCloseMs);
    } else {
      Serial.println(F("Unknown setting. Try: closed, open, autoclose"));
    }
  } else if (!strcmp(cmd, "save")) {
    saveSettings();
  } else if (!strcmp(cmd, "defaults")) {
    restoreDefaults();
    Serial.println(F("Defaults restored (use \"save\" to keep them)."));
  } else {
    Serial.println(F("Unknown command. Type \"help\"."));
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') {
      if (lineLen) {
        lineBuf[lineLen] = 0;
        handleCommand(lineBuf);
        lineLen = 0;
      }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

// ---------------- Button ----------------
#if USE_BUTTON
void pollButton() {
  uint32_t now = millis();
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != buttonLastRead) {
    buttonLastRead = reading;
    buttonChangedMs = now;
  }
  if (now - buttonChangedMs < BUTTON_DEBOUNCE_MS) return;

  if (reading != buttonStable) {
    buttonStable = reading;
    if (buttonStable == LOW) {            // pressed
      buttonPressedMs = now;
      buttonHoldHandled = false;
    } else if (!buttonHoldHandled) {      // released after a short press
      drop("button");
    }
  }
  // long hold toggles arm state
  if (buttonStable == LOW && !buttonHoldHandled && now - buttonPressedMs >= BUTTON_HOLD_MS) {
    buttonHoldHandled = true;
    setArmed(!armed);
  }
}
#endif

// ---------------- Web server ----------------
void startNetwork() {
#if USE_DHCP
  if (!Ethernet.begin()) Serial.println(F("Ethernet failed to start."));
#else
  if (!Ethernet.begin(STATIC_IP, STATIC_MASK, STATIC_GATEWAY)) {
    Serial.println(F("Ethernet failed to start."));
  }
#endif
  server.begin();
}

// Prints the address once it is known and starts mDNS (http://payload.local).
void pollNetwork() {
  bool haveIp = Ethernet.linkState() && uint32_t(Ethernet.localIP()) != 0;
  if (haveIp && !networkReported) {
    networkReported = true;
    if (MDNS.begin(HOSTNAME)) MDNS.addService("_http", "_tcp", HTTP_PORT);
    Serial.print(F("Web GUI: http://"));
    Serial.print(Ethernet.localIP());
    Serial.print(F("  or  http://"));
    Serial.print(HOSTNAME);
    Serial.println(F(".local"));
  } else if (!haveIp && networkReported) {
    networkReported = false;
    MDNS.end();
    Serial.println(F("Ethernet link lost."));
  }
}

// Reads one line (without CR/LF). Returns false on timeout/disconnect.
bool readHttpLine(EthernetClient &c, char *buf, size_t size, uint32_t deadline) {
  size_t n = 0;
  while ((int32_t)(millis() - deadline) < 0) {
    if (c.available()) {
      char ch = c.read();
      if (ch == '\r') continue;
      if (ch == '\n') { buf[n] = 0; return true; }
      if (n < size - 1) buf[n++] = ch;
    } else if (!c.connected()) {
      break;
    } else {
      yield();
    }
  }
  buf[n] = 0;
  return false;
}

// Finds name=value in a query string ("a=1&b=2"). Returns true if found.
bool queryParam(const char *query, const char *name, long &out) {
  size_t len = strlen(name);
  const char *p = query;
  while (p && *p) {
    if (!strncmp(p, name, len) && p[len] == '=') {
      out = atol(p + len + 1);
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

void sendResponse(EthernetClient &c, int code, const char *type, const char *body) {
  const char *reason = code == 200 ? "OK" : code == 401 ? "Unauthorized"
                     : code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed"
                     : code == 409 ? "Conflict" : "Bad Request";
  char header[192];
  size_t bodyLen = strlen(body);
  int n = snprintf(header, sizeof(header),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %u\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n",
                   code, reason, type, (unsigned)bodyLen);
  c.writeFully(header, n);
  c.writeFully(body, bodyLen);
  c.flush();
}

void sendStatus(EthernetClient &c, int code, const char *error) {
  char json[320];
  snprintf(json, sizeof(json),
           "{\"armed\":%s,\"open\":%s,\"pos\":%d,\"closedUs\":%u,\"openUs\":%u,"
           "\"autoCloseMs\":%lu,\"drops\":%lu,\"uptimeS\":%lu,\"keyRequired\":%s%s%s%s}",
           armed ? "true" : "false", isOpen ? "true" : "false",
           (int)(currentUs + 0.5f), cfg.closedUs, cfg.openUs,
           (unsigned long)cfg.autoCloseMs, (unsigned long)dropCount,
           (unsigned long)(millis() / 1000), WEB_KEY[0] ? "true" : "false",
           error ? ",\"error\":\"" : "", error ? error : "", error ? "\"" : "");
  sendResponse(c, code, "application/json", json);
}

void handleHttp(EthernetClient &c) {
  uint32_t deadline = millis() + HTTP_TIMEOUT_MS;
  char line[160];
  if (!readHttpLine(c, line, sizeof(line), deadline)) return;

  // Request line: METHOD /path?query HTTP/1.1
  char *method = strtok(line, " ");
  char *target = strtok(NULL, " ");
  if (!method || !target) { sendResponse(c, 400, "text/plain", "Bad request"); return; }
  char path[64];
  char query[96] = "";
  char *q = strchr(target, '?');
  if (q) {
    *q = 0;
    strncpy(query, q + 1, sizeof(query) - 1);
    query[sizeof(query) - 1] = 0;
  }
  strncpy(path, target, sizeof(path) - 1);
  path[sizeof(path) - 1] = 0;
  bool isGet = !strcmp(method, "GET");
  bool isPost = !strcmp(method, "POST");

  // Headers - we only care about X-Key.
  char hdr[160];
  char key[48] = "";
  while (readHttpLine(c, hdr, sizeof(hdr), deadline) && hdr[0]) {
    if (!strncasecmp(hdr, "x-key:", 6)) {
      const char *v = hdr + 6;
      while (*v == ' ') v++;
      strncpy(key, v, sizeof(key) - 1);
      key[sizeof(key) - 1] = 0;
    }
  }

  if (!strcmp(path, "/")) {
    if (!isGet) { sendResponse(c, 405, "text/plain", "Use GET"); return; }
    sendResponse(c, 200, "text/html; charset=utf-8", INDEX_HTML);
    return;
  }
  if (!strcmp(path, "/api/status")) {
    sendStatus(c, 200, nullptr);
    return;
  }
  if (strncmp(path, "/api/", 5)) { sendResponse(c, 404, "text/plain", "Not found"); return; }

  // Everything else under /api/ changes state: POST only, key checked.
  if (!isPost) { sendStatus(c, 405, "use POST"); return; }
  if (WEB_KEY[0] && strcmp(key, WEB_KEY)) { sendStatus(c, 401, "wrong or missing key"); return; }

  const char *action = path + 5;
  long v;
  if (!strcmp(action, "arm")) {
    setArmed(true);
  } else if (!strcmp(action, "disarm")) {
    setArmed(false);
  } else if (!strcmp(action, "drop")) {
    if (!drop("web")) { sendStatus(c, 409, "disarmed - arm first"); return; }
  } else if (!strcmp(action, "open")) {
    openRelease("web manual");
  } else if (!strcmp(action, "close")) {
    closeRelease();
  } else if (!strcmp(action, "jog")) {
    if (!queryParam(query, "us", v)) { sendStatus(c, 400, "missing us"); return; }
    moveNow(clampUs(v));
  } else if (!strcmp(action, "set")) {
    if (queryParam(query, "closed", v)) setClosed(v);
    if (queryParam(query, "open", v)) setOpen(v);
    if (queryParam(query, "autoclose", v)) setAutoClose(v);
  } else if (!strcmp(action, "save")) {
    saveSettings();
  } else if (!strcmp(action, "defaults")) {
    restoreDefaults();
  } else {
    sendStatus(c, 404, "unknown action");
    return;
  }
  sendStatus(c, 200, nullptr);
}

void pollWeb() {
  // available() only hands over connections that have sent data, so idle
  // browser pre-connections never stall the loop.
  EthernetClient client = server.available();
  if (!client) return;
  handleHttp(client);
  client.stop();
}

// ---------------- LED ----------------
void updateLed() {
  // off = disarmed, slow blink = armed, solid = open
  if (isOpen) digitalWrite(LED_PIN, HIGH);
  else if (armed) digitalWrite(LED_PIN, (millis() / 500) % 2);
  else digitalWrite(LED_PIN, LOW);
}

// ---------------- Setup / loop ----------------
void setup() {
  pinMode(LED_PIN, OUTPUT);
#if USE_BUTTON
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif

  loadSettings();

  // Attach with wide limits so writeMicroseconds isn't clamped to 544-2400.
  servo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  moveNow(cfg.closedUs);   // start closed

  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 1500) {}  // wait briefly for USB, don't block
  Serial.println(F("MG996R Payload Release ready (DISARMED). Type \"help\"."));

  startNetwork();
}

void loop() {
  pollSerial();
  pollNetwork();
  pollWeb();
#if USE_BUTTON
  pollButton();
#endif

  // Auto-close after a drop.
  if (isOpen && autoClosePending && millis() - openedAtMs >= cfg.autoCloseMs) {
    closeRelease();
  }

  updateSlew();
  updateLed();
}
