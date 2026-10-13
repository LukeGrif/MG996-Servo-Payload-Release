# MG996R Servo Payload Release – Teensy 4.1 (Ethernet web control)

Arduino IDE sketch for an MG996/MG996R servo-powered payload release (dropper). It's controlled by a Teensy 4.1 and triggered from a web page the Teensy serves over its built-in Ethernet.

Sketch: [`PayloadRelease/PayloadRelease.ino`](PayloadRelease/PayloadRelease.ino) (the web page is in `PayloadRelease/WebPage.h`)

## Features
- **Web control page** at `http://payload.local` (or the Teensy's IP address). It has Arm/Disarm, a big DROP button with a confirmation prompt, Open/Close, live status, and calibration with a jog slider. It works on a phone.
- USB serial commands and an optional push button also work
- Boots **disarmed**. Every drop needs the unit armed first
- Web actions use POST, so a browser prefetch or link preview can't trigger a drop
- Optional access key (`WEB_KEY`) for web actions
- Opens fast and closes slowly, so the latch doesn't snap or jam
- Closes automatically after a drop (3 s by default, adjustable, can be turned off)
- Calibration is saved to EEPROM
- Status LED (pin 13): off = disarmed, blinking = armed, solid = open

## Hardware
- Teensy 4.1 **with the Ethernet kit**: the RJ45 MagJack adapter connected to the Teensy's 6-pin Ethernet header
- An Ethernet cable to your router or switch. The computer or phone you control it from must be on the same network.

### Wiring

| Servo wire      | Connect to |
|-----------------|-----------|
| Orange (signal) | Teensy pin **9** |
| Red (+V)        | **External 5–6 V supply, 3 A or more** (a BEC or UBEC) |
| Brown (GND)     | Supply GND **and** Teensy GND |

| Optional        | Connect to |
|-----------------|-----------|
| Push button     | Pin **2** ↔ GND (uses the internal pull-up) |

⚠️ **Don't power the MG996R from the Teensy's 5 V/VIN or 3.3 V pins.** Under load or at stall it can draw about 2.5 A, which will brown out or damage the Teensy. The grounds **must** be connected together.

## Setup
1. Install [Teensyduino](https://www.pjrc.com/teensy/td_download.html) 1.58 or newer into the Arduino IDE. It includes the `Servo`, `EEPROM` and `QNEthernet` libraries. If `QNEthernet.h` isn't found, install **QNEthernet** from the Library Manager.
2. Open `PayloadRelease/PayloadRelease.ino` and select **Tools → Board → Teensy 4.1** and **USB Type → Serial**.
3. Upload, then open the Serial Monitor at **115200** baud. Once the Teensy gets an address it prints a line like:
   ```
   Web GUI: http://192.168.1.77  or  http://payload.local
   ```
4. Open that address in a browser. If `payload.local` doesn't resolve (some Windows and Android setups don't support mDNS), use the IP address. You can also type `status` in the Serial Monitor to see it.

### Network options (top of the sketch)
- `USE_DHCP 1` (default) gets an address from your router automatically. To use a fixed address, set it to `0` and edit `STATIC_IP`, `STATIC_MASK` and `STATIC_GATEWAY`.
- `HOSTNAME` sets the mDNS name (`payload` → `payload.local`).
- `WEB_KEY` is empty by default, so no key is needed. If you set it, enter the same key in the page's **Settings** box. The page remembers it in your browser.

> 🔒 **Keep it on your local network.** There's no HTTPS, and the key is only a basic lock. Don't port-forward it to the internet. If you need to control it from somewhere else, connect to your home network through a VPN (for example WireGuard or Tailscale on the router or another machine on that network).

## Calibration (do this before fitting a payload)
The default positions (closed = 1000 µs, open = 2000 µs) are guesses. Open **Calibration** on the web page:
1. Drag the **Jog** slider until the latch is fully closed without the servo straining or buzzing, then tap **Use as closed**.
2. Drag it until the payload falls free, then tap **Use as open**.
3. Set the auto-close delay and tap **Save to EEPROM**.

If the servo hums or gets hot while it's closed, it's pushing against a hard stop. Move the closed value back by 20–50 µs.

## Web API
You can also script it with `curl`. Add `-H "X-Key: <key>"` if `WEB_KEY` is set.

| Method | Path | Action |
|---|---|---|
| GET  | `/api/status` | State as JSON |
| POST | `/api/arm`, `/api/disarm` | Arm / disarm |
| POST | `/api/drop` | Drop (returns 409 if disarmed) |
| POST | `/api/open`, `/api/close` | Manual open / close (no auto-close) |
| POST | `/api/jog?us=1500` | Move to a raw position |
| POST | `/api/set?closed=1050&open=1900&autoclose=3000` | Change settings (any subset) |
| POST | `/api/save`, `/api/defaults` | Save to EEPROM / restore the defaults |

```bash
curl -X POST http://payload.local/api/arm
curl -X POST http://payload.local/api/drop
```

## Serial commands
| Command | Action |
|---|---|
| `arm` / `disarm` | Enable or disable drops |
| `drop` | Release the payload (must be armed) |
| `open` / `close` | Move the release manually. Manual `open` does not auto-close, so you can load the payload |
| `status` | Show the state, positions and IP address |
| `jog <us>` | Move the servo to a raw position (500–2500) |
| `set closed <us>` / `set open <us>` | Set the positions |
| `set autoclose <ms>` | Auto-close delay after a drop (0 = off) |
| `save` / `defaults` | Save to EEPROM / restore the defaults |

## Button (optional)
- **Short press:** drop (if armed)
- **Hold for 2 s:** toggle armed/disarmed

Set `USE_BUTTON` to `0` if you don't fit one.
