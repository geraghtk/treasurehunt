# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP32-C3-based BLE Treasure Hunt firmware running on a **Waveshare ESP32-C3-Zero**. A handheld device scans for BLE beacons at physical locations; when the device is close enough to the correct beacon (RSSI threshold), it advances to the next clue displayed on a GxEPD2 e-Paper display (Waveshare 4.2" B&W, GDEY042T81/SSD1683). A 16-LED NeoPixel ring gives live proximity feedback. Progress is persisted to ESP32 NVRAM.

## Build & Flash Commands

This is a PlatformIO project. There is no npm/make — use the `pio` CLI or the PlatformIO IDE extension for VS Code.

```bash
# Build
platformio run -e esp32c3

# Build and flash to connected ESP32-C3
platformio run -e esp32c3 --target upload

# Open serial monitor (115200 baud)
platformio run -e esp32c3 --target monitor

# Build, flash, and monitor in one step
platformio run -e esp32c3 --target upload --target monitor

# Clean build artifacts
platformio run --target clean
```

## Architecture

All firmware lives in `src/BLE_TreasureHunt.ino`. The project has no custom libraries (`lib/` is empty).

### Core subsystems

**Clue data** — A `clues[]` array holds all game state: beacon MAC address, riddle text, and a pointer to a bitmap image. Bitmaps are defined in `src/ImageData.h` (large file, ~2950 lines).

**BLE Scanner** — Non-blocking async scan (2-second windows via `pBLEScan->start(SCAN_DURATION, scanCompleteCallback, false)`) so the main loop can keep animating the ring while scanning. Compares detected MAC addresses and RSSI against the current clue's beacon. `RSSI_THRESHOLD` is `-55` dBm (less negative = physically closer required — default requires the beacon within ~30 cm). A `ADVANCE_COOLDOWN_MS` (3s) lockout after a clue advance prevents a single close encounter from skipping past a clue if the next beacon also happens to be in range.

**BLE Server** — Exposes a writable BLE characteristic for manual clue override (useful for testing without physical beacons). Service UUID: `12345678-1234-1234-1234-1234567890ab`. Accepts `"0"` (reset to title screen), `"1"` (start hunt from saved progress), or a numeric clue index (`"0"`–`"5"`) to jump directly to a specific clue.

**E-Paper display** — GxEPD2_420_GDEY042T81 / SSD1683 (400×300 px, rotated to portrait 300×400). Pins: CS=7, DC=1, RST=2, BUSY=3, CLK=4, DIN=6. Displays title on boot, current clue image + riddle text during hunt, and a "You found the treasure!" finale after the last beacon. Display hibernates between refreshes to reduce power draw. **E-paper refresh takes ~4 seconds** — it must NOT run on the BLE task, so BLE callbacks set a `pendingDraw` flag (`DRAW_START` / `DRAW_CLUE` / `DRAW_FINALE`) plus `pendingStopScan`, and `loop()` services them on the Arduino task.

**NeoPixel ring** — 16-LED ring on GPIO 21 (Adafruit NeoPixel library, GRB, 800 kHz). Three display modes, driven exclusively from `loop()` to avoid fighting with the BLE callback:
- **Idle** — single rotating dim blue pixel (~150 ms tick rate). Shown on title screen and while scanning with no target beacon seen.
- **Proximity bar graph** — red→green gradient, number of lit LEDs scales linearly with RSSI over `[RSSI_RING_MIN=-100, RSSI_RING_MAX=-55]`. Shown while target beacon is in range.
- **Victory rainbow** — HSV rainbow spin. Shown when `huntComplete` is set.

Switch between idle and bar graph is gated by `TARGET_GRACE_MS` (4000 ms) since last detection — the grace window must exceed the beacon's advertising interval or the ring flickers between modes.

**Persistence** — `Preferences` library stores `clueIndex` under the key `"progress"`. Survives power cycles. `huntComplete` is NOT persisted — power-cycling at the finale returns to the title; sending `"1"` will then resume on the last clue and re-trigger the finale when the beacon is found again.

### Clue progression

6 clues in sequence. Beacon sticker labels and MACs are documented in comments at the top of the `clues[]` array in `BLE_TreasureHunt.ino` (Strawberry, Flower, Dog, Cat, Dumbo, Penguin).

### Key dependencies (platformio.ini)

- `zinggjm/GxEPD2 @ ^1.5.6` — e-Paper driver (must be ≥1.5.6 for GDEY042T81 partial update fix)
- `adafruit/Adafruit GFX Library @ ^1.11.0` — fonts and graphics primitives
- `adafruit/Adafruit NeoPixel @ ^1.12.0` — driver for the 16-LED proximity ring on GPIO 21

## Arduino/PlatformIO Workflow

**pio location on this machine:**
```
C:/Users/Kevin/AppData/Local/Packages/PythonSoftwareFoundation.Python.3.13_qbz5n2kfra8p0/LocalCache/local-packages/Python313/Scripts/pio.exe
```

**Serial port:** COM4 (ESP32-C3-Zero via native USB CDC). Always kill any running `pio.exe` monitor before flashing (`taskkill /F /IM pio.exe`). The C3-Zero requires a data-capable USB cable (not power-only).

**Diagnostic/test sketches:** Never overwrite `BLE_TreasureHunt.ino` for temporary testing. Save diagnostic code to a separate file with a `.txt` extension (e.g. `src/MyTool.ino.txt`) — PlatformIO ignores `.txt` files. A reusable beacon scanner and presence checker is saved in `src/BLE_Scanner.ino.txt`.
