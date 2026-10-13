/*
  MG996R Servo Payload Release - Teensy 4.1
  ------------------------------------------
  Drives an MG996/MG996R servo-powered payload release (drop) mechanism.

  Ways to trigger a drop:
    1. USB Serial commands (115200 baud, newline terminated) - type "help"
    2. A push button on BUTTON_PIN (short press = drop, hold 2 s = arm/disarm)
    3. An RC receiver channel on RC_PIN (e.g. a switch on your transmitter)

  Safety:
    - The unit boots DISARMED. Serial and button drops need the unit armed.
    - The RC trigger uses the transmitter switch as the safety: it only fires
      after the switch has been seen in the LOW position since boot, so a
      switch left high at power-up will not drop the payload.
    - If the RC signal is lost, nothing happens (it never drops on failsafe).

  Wiring (see README.md):
    Servo signal (orange) -> Teensy pin 9
    Servo +V (red)        -> external 5-6 V supply, 3 A or more (NOT Teensy 5V/3.3V)
    Servo GND (brown)     -> supply GND AND Teensy GND (common ground required)
    Button                -> pin 2 to GND (internal pull-up used)
    RC receiver signal    -> pin 3 (receiver GND to Teensy GND)

  Calibration values are stored in EEPROM, so set them once with "set" + "save".
*/

#include <Servo.h>
#include <EEPROM.h>

// ---------------- User configuration ----------------
const uint8_t SERVO_PIN  = 9;
const uint8_t BUTTON_PIN = 2;
const uint8_t RC_PIN     = 3;
const uint8_t LED_PIN    = LED_BUILTIN;   // pin 13 on Teensy 4.1

#define USE_BUTTON    1   // set to 0 if no button is fitted
#define USE_RC_INPUT  1   // set to 0 if no RC receiver is connected

// Default positions in microseconds. MG996R covers ~180 deg over 500-2500 us.
// Use the "jog" command to find the right values for your mechanism.
const uint16_t DEFAULT_CLOSED_US     = 1000;  // hook/latch closed (holding payload)
const uint16_t DEFAULT_OPEN_US       = 2000;  // hook/latch open (payload released)
const uint32_t DEFAULT_AUTOCLOSE_MS  = 3000;  // 0 = stay open after a drop

const uint16_t SERVO_MIN_US = 500;            // hard limits for any command
const uint16_t SERVO_MAX_US = 2500;
const float    CLOSE_SPEED_US_PER_MS = 1.0f;  // slow close so it doesn't snap/jam

const uint16_t RC_TRIGGER_HIGH_US = 1700;     // above this = "drop"
const uint16_t RC_TRIGGER_LOW_US  = 1300;     // below this = "safe / re-arm switch"
const uint32_t RC_TIMEOUT_MS      = 100;      // no pulses for this long = signal lost

const uint32_t BUTTON_DEBOUNCE_MS = 30;
const uint32_t BUTTON_HOLD_MS     = 2000;

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

// RC input (written in ISR)
volatile uint32_t rcRiseUs = 0;
volatile uint16_t rcPulseUs = 0;
volatile uint32_t rcLastPulseMs = 0;
bool rcSeenLow = false;      // switch must be seen low before it can trigger
bool rcWasHigh = false;

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

// A "drop" is an open followed by auto-close (if enabled). Needs to be armed
// unless it comes from the RC switch (see header comment).
void drop(const char *source, bool needArm) {
  if (needArm && !armed) {
    Serial.println(F("Refused: DISARMED. Send \"arm\" first."));
    return;
  }
  dropCount++;
  openRelease(source);
  autoClosePending = cfg.autoCloseMs > 0;
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
#if USE_RC_INPUT
  noInterrupts();
  uint16_t p = rcPulseUs;
  uint32_t last = rcLastPulseMs;
  interrupts();
  Serial.print(F("RC input:     "));
  if (p != 0 && millis() - last < RC_TIMEOUT_MS) { Serial.print(p); Serial.print(F(" us")); }
  else Serial.print(F("no signal"));
  Serial.println(rcSeenLow ? F("") : F(" (waiting for switch LOW)"));
#endif
}

void printHelp() {
  Serial.println(F("Commands:"));
  Serial.println(F("  arm | disarm          enable/disable drops"));
  Serial.println(F("  drop                  release payload (must be armed)"));
  Serial.println(F("  open | close          move release manually (no auto-close)"));
  Serial.println(F("  status                show current state"));
  Serial.println(F("  jog <us>              move servo to raw position (calibration)"));
  Serial.println(F("  set closed <us>       set closed position (500-2500)"));
  Serial.println(F("  set open <us>         set open position (500-2500)"));
  Serial.println(F("  set autoclose <ms>    auto-close delay after drop, 0 = off"));
  Serial.println(F("  save                  store settings in EEPROM"));
  Serial.println(F("  defaults              restore default settings (not saved)"));
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
    drop("serial", true);
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
      cfg.closedUs = clampUs(v);
      if (!isOpen) moveSlow(cfg.closedUs);
      Serial.print(F("Closed = ")); Serial.println(cfg.closedUs);
    } else if (!strcmp(arg1, "open")) {
      cfg.openUs = clampUs(v);
      if (isOpen) moveNow(cfg.openUs);
      Serial.print(F("Open = ")); Serial.println(cfg.openUs);
    } else if (!strcmp(arg1, "autoclose")) {
      cfg.autoCloseMs = v < 0 ? 0 : (uint32_t)v;
      Serial.print(F("Auto-close = ")); Serial.println(cfg.autoCloseMs);
    } else {
      Serial.println(F("Unknown setting. Try: closed, open, autoclose"));
    }
  } else if (!strcmp(cmd, "save")) {
    saveSettings();
  } else if (!strcmp(cmd, "defaults")) {
    cfg.closedUs = DEFAULT_CLOSED_US;
    cfg.openUs = DEFAULT_OPEN_US;
    cfg.autoCloseMs = DEFAULT_AUTOCLOSE_MS;
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
      drop("button", true);
    }
  }
  // long hold toggles arm state
  if (buttonStable == LOW && !buttonHoldHandled && now - buttonPressedMs >= BUTTON_HOLD_MS) {
    buttonHoldHandled = true;
    setArmed(!armed);
  }
}
#endif

// ---------------- RC receiver input ----------------
#if USE_RC_INPUT
void rcIsr() {
  uint32_t now = micros();
  if (digitalReadFast(RC_PIN)) {
    rcRiseUs = now;
  } else {
    uint32_t w = now - rcRiseUs;
    if (w >= 800 && w <= 2200) {          // ignore glitches
      rcPulseUs = w;
      rcLastPulseMs = millis();
    }
  }
}

void pollRc() {
  noInterrupts();
  uint16_t p = rcPulseUs;
  uint32_t last = rcLastPulseMs;
  interrupts();

  if (p == 0 || millis() - last >= RC_TIMEOUT_MS) return;   // no/lost signal: do nothing

  if (p <= RC_TRIGGER_LOW_US) {
    if (!rcSeenLow) Serial.println(F("RC switch seen LOW - RC trigger ready."));
    rcSeenLow = true;
    if (rcWasHigh && isOpen) closeRelease();       // flicking switch back closes it
    rcWasHigh = false;
  } else if (p >= RC_TRIGGER_HIGH_US && rcSeenLow && !rcWasHigh) {
    rcWasHigh = true;                              // edge-triggered: one drop per flick
    drop("rc", false);
  }
}
#endif

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
#if USE_RC_INPUT
  pinMode(RC_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(RC_PIN), rcIsr, CHANGE);
#endif

  loadSettings();

  // Attach with wide limits so writeMicroseconds isn't clamped to 544-2400.
  servo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  moveNow(cfg.closedUs);   // start closed

  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 1500) {}  // wait briefly for USB, don't block flight
  Serial.println(F("MG996R Payload Release ready (DISARMED). Type \"help\"."));
}

void loop() {
  pollSerial();
#if USE_BUTTON
  pollButton();
#endif
#if USE_RC_INPUT
  pollRc();
#endif

  // Auto-close after a drop.
  if (isOpen && autoClosePending && millis() - openedAtMs >= cfg.autoCloseMs) {
    closeRelease();
  }

  updateSlew();
  updateLed();
}
