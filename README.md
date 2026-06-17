# ESP32 BLE Treasure Hunt

A handheld treasure hunt device (the "book") built on the **Waveshare ESP32-C3-Zero**. Players carry the device around physical locations; as they approach each BLE beacon the NeoPixel ring gives live proximity feedback, and when they're close enough the next clue appears on the e-Paper display. Finding the last beacon reveals the finale clue and triggers a companion prop to speak.

Based on the original concept by [Alastair Aitchison / Playful Technology](https://playfultech.io/).

---

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | Waveshare ESP32-C3-Zero |
| Display | Waveshare 4.2″ B/W e-Paper (GDEY042T81 / SSD1683), 400×300 px, used in portrait (300×400) |
| Proximity ring | 16-LED NeoPixel ring |
| Beacons | 6 × Bluetooth BLE sticker beacons |

### Pin assignments (ESP32-C3-Zero)

| Signal | GPIO |
|--------|------|
| e-Paper CS | 7 |
| e-Paper DC | 1 |
| e-Paper RST | 2 |
| e-Paper BUSY | 3 |
| SPI CLK | 4 |
| SPI DIN | 6 |
| NeoPixel data | 21 |

---

## How it works

1. **Boot** — title screen is shown on the e-Paper display. The device advertises over BLE as **`TreasureHunt`** and waits for a start command (it does not begin the hunt on its own).
2. **Start** — writing `"1"` over BLE (see [remote override](#ble-remote-override)) begins the hunt from saved progress and shows the current clue.
3. **BLE scan** — the device runs continuous 2-second async BLE scan windows, looking for the MAC address of the current target beacon. Scanning is non-blocking so the ring keeps animating.
4. **Proximity feedback** — the NeoPixel ring has three modes, all driven from `loop()`:
   - **Idle** — a single dim-blue pixel rotates while searching (no target beacon in range).
   - **Proximity bar** — a red→green bar that fills as RSSI rises across `[-100, -55]` dBm, shown while the target beacon is in range. A 4 s grace window (`TARGET_GRACE_MS`) keeps the bar steady between beacon advertisements.
   - **Victory rainbow** — an HSV rainbow spin once the hunt is complete.
5. **Clue advance** — when RSSI ≥ `-55` dBm (`RSSI_THRESHOLD`, roughly ≤30 cm), the display refreshes with the next clue image and riddle, and the next beacon becomes the target. A 3 s cooldown (`ADVANCE_COOLDOWN_MS`) prevents one close encounter from skipping a clue.
6. **Finale** — after the 6th beacon is found, the e-Paper shows the *whisper-stones* finale verse and the ring plays the victory rainbow. The device then connects to the **VoiceRecognizer** prop and asks it to speak (see [Companion props](#companion-props)).
7. **Persistence** — the current clue index is saved to NVRAM (`Preferences`, key `"progress"`) and survives power cycles. `huntComplete` is **not** persisted: power-cycling at the finale returns to the title screen, and writing `"1"` resumes on the last clue and re-triggers the finale when that beacon is found again.

> **E-paper note:** a full refresh takes ~4 s and must not run on the BLE task. BLE callbacks set a `pendingDraw` flag and `loop()` performs the draw on the Arduino task. The display hibernates between refreshes to save power.

### BLE remote override

The book exposes a writable characteristic so an operator (the [BTEscapeRoomController](#companion-props) web app, nRF Connect, Bluefy, …) can drive it without physical beacons. Multiple centrals may connect at once — the book keeps advertising after each connection so a second phone or peer prop can still find it.

- Service UUID: `12345678-1234-1234-1234-1234567890ab`
- Clue characteristic (write): `abcd1234-abcd-1234-abcd-12345678abcd`

| Write value | Effect |
|-------------|--------|
| `"0"` | Reset to title screen, clear progress (`progress = 0`) |
| `"1"` | Start / resume the hunt from saved progress |
| `"2"`–`"5"` | Jump directly to that clue index (`0`–`5` are valid, but `0` and `1` are intercepted as reset/start above, so direct jumps are `2`–`5`) |
| `"done"` | Force completion: show the finale verse, run the victory ring, and trigger the VoiceRecognizer prop (re-arms the one-shot voice latch so it can fire again) |

> iOS discovery note: the device advertises with a split packet — the 128-bit service UUID in the primary advertisement (so CoreBluetooth/Bluefy service-filtered scans match it) and the complete name `TreasureHunt` in the scan response.

---

## Clue map

Clues advance in the order below (the order of the `clues[]` array in `BLE_TreasureHunt.ino`). Each clue is tied to one physical beacon sticker / MAC address.

| Order | Sticker | Beacon MAC | Riddle | Image |
|:-----:|---------|------------|--------|-------|
| 1 | Flower | `e4:ce:26:90:96:cf` | *Step with care / a tree once stood, / yet now lies down / above the flood.* | `stonehenge` |
| 2 | Strawberry | `f2:6c:8d:69:56:5d` | *Your path begins / at a living arc, / from earth to sky / then back to bark.* | `smallslide` |
| 3 | Dog | `d8:fe:b6:49:be:57` | *Beneath the trees / a gorge runs deep, / a winding trail / where secrets sleep.* | `exercise` |
| 4 | Cat | `d5:65:c6:25:a1:5a` | *An ancient guard / its heart long gone, / a hollow soul / to journey on.* | `windyslide` |
| 5 | Dumbo | `e6:b9:7d:7d:4b:8e` | *Full and round, / its branches spread, / a bushy crown / above your head.* | `tennis` |
| 6 | Penguin | `d6:e7:1f:86:0b:56` | *Where paws have worn / the ground down flat, / a loyal friend / returns to that.* | `picnic` |

Finding beacon 6 (Penguin) shows the finale verse:

> *Now all whisper stones are found,*
> *But whispers barely make a sound.*
> *To make their shy voices complete,*
> *Bring the whispers to his feet.*

---

## Companion props

The treasure hunt is part of a small ecosystem of BLE props. Its direct and shared relationships:

- **[VoiceRecognizer](../voice-recognizer)** *(direct BLE link)* — when the hunt completes and the book gets close (RSSI ≥ `-60` dBm), the book acts as a BLE **central**, connects to the VoiceRecognizer, and writes `"p10"` to its Nordic UART RX characteristic. That tells the prop to play track 10 (`/mp3/0010.mp3`, the "Oracle" finale line) — the payoff to the finale verse's "bring the whispers to his feet." This fires once per power cycle.
  - VoiceRecognizer NUS service: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
  - RX characteristic (central → prop): `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- **[BTEscapeRoomController](../BTEscapeRoomController)** *(operator control)* — a Web Bluetooth app that connects to the book's service `12345678-…` and sends the override commands above (its "Complete hunt" button sends `"done"`).
- **[crate](../crate)** *(shared beacons, no direct link)* — a multi-NFC-reader prop that uses the **same six physical sticker beacons** (Strawberry, Flower, Dog, Cat, Dumbo, Penguin) and mirrors this project's clue ordering. There is no BLE traffic between the crate and the book; they simply share the physical props.

---

## Build & flash

This is a [PlatformIO](https://platformio.org/) project targeting the `esp32c3` environment.

```bash
# Build
pio run -e esp32c3

# Build and upload (COM4 by default — edit platformio.ini if different)
pio run -e esp32c3 --target upload

# Open serial monitor (115200 baud)
pio run -e esp32c3 --target monitor

# Build, upload, and monitor in one step
pio run -e esp32c3 --target upload --target monitor
```

> **Note:** The ESP32-C3-Zero uses native USB CDC. Always kill any running `pio` monitor process before flashing, and use a **data-capable** USB cable (not charge-only).

---

## Project structure

```
src/
  BLE_TreasureHunt.ino       # Main firmware
  ImageData.h                # Clue bitmaps (generated with image2cpp / img2epd)
  BLE_Scanner.ino.txt        # Standalone BLE scanner utility (not compiled)
  GDEY042T81_GxEPD2.ino.txt  # e-Paper diagnostic sketch (not compiled)
tools/
  img2epd/img2epd.py         # Convert images to e-Paper bitmap arrays
  web-monitor/               # Browser-based serial monitor
backup/                      # Earlier firmware versions
platformio.ini
```

> Diagnostic / test sketches are stored with a `.txt` extension so PlatformIO ignores them.

---

## Dependencies

| Library | Version | Purpose |
|---------|---------|---------|
| [GxEPD2](https://github.com/ZinggJM/GxEPD2) | ≥ 1.5.6 | e-Paper display driver |
| [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) | ≥ 1.11.0 | Fonts and graphics primitives |
| [Adafruit NeoPixel](https://github.com/adafruit/Adafruit_NeoPixel) | ≥ 1.12.0 | NeoPixel ring driver |

ESP32 BLE Arduino and Preferences ship with the Arduino-ESP32 core. All dependencies install automatically via `platformio.ini`.

---

## Adding or replacing clue images

1. Prepare a **grayscale** image (clue images are 300 × 260 px; see `tools/img2epd/IMAGE_PROMPT.md` for the photo→line-art prompt and sizing).
2. Run `tools/img2epd/img2epd.py` to convert it to a C byte array.
3. Paste the output into `src/ImageData.h` and update the corresponding entry in the `clues[]` array in `BLE_TreasureHunt.ino`.
