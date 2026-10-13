/*
  Servo hardware test - Teensy 4.1, servo signal on pin 9.
  No libraries, no network: generates servo pulses by hand and swings the
  servo between its two ends every 2 seconds while the LED blinks.
  If the LED blinks but the servo doesn't move, the problem is wiring/power.
*/
const int PIN = 9;

void pulseFor(int us, int ms) {
  for (int t = 0; t < ms; t += 20) {      // 50 Hz servo frame
    digitalWrite(PIN, HIGH);
    delayMicroseconds(us);
    digitalWrite(PIN, LOW);
    delay(20 - us / 1000);
  }
}

void setup() {
  pinMode(PIN, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(115200);
}

void loop() {
  Serial.println("-> 600 us");
  digitalWrite(LED_BUILTIN, HIGH);
  pulseFor(600, 2000);

  Serial.println("-> 2400 us");
  digitalWrite(LED_BUILTIN, LOW);
  pulseFor(2400, 2000);
}
