# ESP32 BLE Treasure Hunt

A handheld treasure hunt device built on the **Waveshare ESP32-C3-Zero**. Players carry the device around physical locations; as they approach each BLE beacon the NeoPixel ring gives live proximity feedback, and when they're close enough the next clue appears on the e-Paper display.

Based on the original concept by [Alastair Aitchison / Playful Technology](https://playfultech.io/).

---

## Hardware

| Component | Part |
|-----------|------|
| Microcontroller | Waveshare ESP32-C3-Zero |
| Display | Waveshare 4.2″ B/W e-Paper (GDEY042T81 / SSD1683) |
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

1. **Boot** — title screen is shown on the e-Paper display.
2. **BLE scan** — the device runs continuous 2-second BLE scan windows, looking for the MAC address of the current target beacon.
3. **Proximity feedback** — when the target beacon is detected, the NeoPixel ring switches from an idle blue spin to a **red→green proximity bar** that fills as the player gets closer (RSSI range –100 to –55 dBm).
4. **Clue advance** — when RSSI ≥ –55 dBm (roughly ≤30 cm), the display refreshes with the next clue image and riddle text, and the new target beacon becomes active.
5. **Finale** — after the 6th beacon is found, a "You found the treasure!" screen is shown and the ring plays a rainbow animation.
6. **Persistence** — progress (current clue index) is saved to NVRAM and survives power cycles.

### BLE remote override

A writable BLE characteristic lets you control the device remotely (useful for testing):

- Service UUID: `12345678-1234-1234-1234-1234567890ab`
- Write `"0"` → reset to title screen
- Write `"1"` → start / resume hunt from saved progress
- Write `"2"`–`"6"` → jump directly to a specific clue

---

## Beacon map

| Label | MAC address |
|-------|-------------|
| Strawberry | `f2:6c:8d:69:56:5d` |
| Flower | `e4:ce:26:90:96:cf` |
| Dog | `d8:fe:b6:49:be:57` |
| Cat | `d5:65:c6:25:a1:5a` |
| Dumbo | `e6:b9:7d:7d:4b:8e` |
| Penguin | `d6:e7:1f:86:0b:56` |

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
  ImageData.h                # Clue bitmaps (generated with image2cpp)
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

All installed automatically by PlatformIO via `platformio.ini`.

---

## Adding or replacing clue images

1. Prepare a **grayscale** image at 300 × N px (width fixed; height varies per clue).
2. Run `tools/img2epd/img2epd.py` to convert it to a C byte array.
3. Paste the output into `src/ImageData.h` and update the corresponding entry in the `clues[]` array in `BLE_TreasureHunt.ino`.
