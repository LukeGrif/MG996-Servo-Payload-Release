# MG996R Servo Payload Release – Teensy 4.1

Arduino IDE sketch for an MG996/MG996R servo-powered payload release (dropper) on a drone or RC car, controlled by a Teensy 4.1.

Sketch: [`PayloadRelease/PayloadRelease.ino`](PayloadRelease/PayloadRelease.ino)

## Features
- Three ways to trigger a drop: USB serial commands, a push button, or an RC receiver channel (a transmitter switch)
- Boots **disarmed**. Serial and button drops only work once the unit is armed
- The RC trigger only fires after the switch has been seen LOW since power-up, and it never drops if the signal is lost
- Opens fast and closes slowly, so the latch doesn't snap or jam
- Closes automatically after a drop (3 s by default, adjustable, can be turned off)
- Calibration is saved to EEPROM
- Status LED (pin 13): off = disarmed, blinking = armed, solid = open

## Wiring

| Servo wire      | Connect to |
|-----------------|-----------|
| Orange (signal) | Teensy pin **9** |
| Red (+V)        | **External 5–6 V supply, 3 A or more** (a BEC or UBEC) |
| Brown (GND)     | Supply GND **and** Teensy GND |

| Optional        | Connect to |
|-----------------|-----------|
| Push button     | Pin **2** ↔ GND (uses the internal pull-up) |
| RC receiver channel signal | Pin **3** (receiver GND to Teensy GND) |

⚠️ **Don't power the MG996R from the Teensy's 5 V/VIN or 3.3 V pins.** Under load or at stall it can draw about 2.5 A, which will brown out or damage the Teensy. The grounds **must** be connected together. The Teensy's 3.3 V signal drives the MG996R fine.

## Setup
1. Install [Teensyduino](https://www.pjrc.com/teensy/td_download.html) into the Arduino IDE. The `Servo` and `EEPROM` libraries come with it.
2. Open `PayloadRelease/PayloadRelease.ino` and select **Tools → Board → Teensy 4.1** and **USB Type → Serial**.
3. Upload, then open the Serial Monitor at **115200** baud with line ending set to **Newline**.

## Calibration (do this before fitting a payload)
The default positions (closed = 1000 µs, open = 2000 µs) are guesses. Every mechanism is different, so tune them first:

```
jog 1000          # move the servo; adjust until the latch is fully closed
jog 1050          # ...without the servo straining/buzzing against a stop
set closed 1050
jog 1900          # find a position where the payload falls free
set open 1900
set autoclose 3000
save
```

If the servo hums or gets hot while it's closed, it's pushing against a hard stop. Move the `closed` value back by 20–50 µs.

## Serial commands
| Command | Action |
|---|---|
| `arm` / `disarm` | Enable or disable drops |
| `drop` | Release the payload (must be armed) |
| `open` / `close` | Move the release manually. Manual `open` does not auto-close, so you can load the payload |
| `status` | Show the state, positions and RC signal |
| `jog <us>` | Move the servo to a raw position (500–2500) |
| `set closed <us>` / `set open <us>` | Set the positions |
| `set autoclose <ms>` | Auto-close delay after a drop (0 = off) |
| `save` / `defaults` | Save to EEPROM / restore the defaults |

## Button
- **Short press:** drop (if armed)
- **Hold for 2 s:** toggle armed/disarmed

## RC receiver
Connect a spare receiver channel that's mapped to a 2-position switch:
- Switch **LOW** (<1300 µs): ready / close
- Switch **HIGH** (>1700 µs): drop (once per flick)

The RC switch counts as its own safety, so it doesn't need `arm`. To change this, or to adjust the thresholds, edit the constants at the top of the sketch. Set `USE_RC_INPUT` or `USE_BUTTON` to `0` if you're not using them.
