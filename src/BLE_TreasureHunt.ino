/**
* Bluetooth BLE Beacon Treasure Hunt
* Copyright 2025 Alastair Aitchison, Playful Technology
*
* Updated for: Waveshare ESP32-C3-Zero + GDEY042T81 (SSD1683)
*/

// DEFINES
// Advertise a BLE service to enable remote override of current clue
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CLUE_CHAR_UUID      "abcd1234-abcd-1234-abcd-12345678abcd"

// ESP32-C3-Zero pin assignments
#define PIN_CS   7
#define PIN_DC   1
#define PIN_RST  2
#define PIN_BUSY 3
#define PIN_CLK  4
#define PIN_DIN  6
#define PIN_NEOPIXEL 21
#define NUM_LEDS 16

// INCLUDES
#include <SPI.h>
// For scanning nearby BLE beacons
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEBeacon.h>
// For advertising BLE characteristic allowing manual override
#include <BLEServer.h>
// For connecting out to the VoiceRecognizer prop to trigger its finale sound
#include <BLEClient.h>
#include <BLEUtils.h>
#include <BLE2902.h>
// For saving progress
#include <Preferences.h>
// e-Paper display library. See https://github.com/ZinggJM/GxEPD2
#include <GxEPD2_BW.h>
// Font from https://github.com/adafruit/Adafruit-GFX-Library/tree/master/Fonts
#include <Fonts/FreeMonoBold12pt7b.h>
// NeoPixel ring for proximity feedback
#include <Adafruit_NeoPixel.h>
// Clue images (as byte arrays, created with https://javl.github.io/image2cpp/)
#include "ImageData.h"

// STRUCTS
// Treasure hunt clues & corresponding beacon UUIDs
struct Clue {
  const uint8_t mac[6];
  const char* text;
  const uint16_t imageWidth;
  const uint16_t imageHeight;
  const unsigned char* imageData;
};

// CONSTANTS
constexpr int SCAN_DURATION = 2;
// Beacon sticker labels and MAC addresses:
//   Strawberry : f2:6c:8d:69:56:5d
//   Flower     : e4:ce:26:90:96:cf
//   Dog        : d8:fe:b6:49:be:57
//   Cat        : d5:65:c6:25:a1:5a
//   Dumbo      : e6:b9:7d:7d:4b:8e
//   Penguin    : d6:e7:1f:86:0b:56
constexpr Clue clues[] = {
    {{0xE4, 0xCE, 0x26, 0x90, 0x96, 0xCF}, "At the start\nwhere you do art", 300, 260, epd_bitmap_picnic},      // Flower
    {{0xF2, 0x6C, 0x8D, 0x69, 0x56, 0x5D}, "Play play play,\nthe most exciting part of today", 300, 260, epd_bitmap_smallslide},        // Strawberry
    {{0xD8, 0xFE, 0xB6, 0x49, 0xBE, 0x57}, "Balance is the key,\nthe voices must be set free.", 300, 260, epd_bitmap_exercise},      // Dog
    {{0xD5, 0x65, 0xC6, 0x25, 0xA1, 0x5A}, "Spiral down,\nlook from the ground", 300, 260, epd_bitmap_windyslide},              // Cat
    {{0xE6, 0xB9, 0x7D, 0x7D, 0x4B, 0x8E}, "Gates and fences,\ndon't forget the benches", 300, 260, epd_bitmap_tennis},             // Dumbo
    {{0xD6, 0xE7, 0x1F, 0x86, 0x0B, 0x56}, "There was a beautiful tree,\nwith kids climbing free.", 300, 260, epd_bitmap_stonehenge}};    // Penguin
constexpr int TOTAL_CLUES = sizeof(clues) / sizeof(Clue);
// Threshold of how strong signal needs to be. Range from -90 (weak) to 0 (strong)
// Higher values (i.e. less negative, closer to zero) mean the player will need to get closer to each beacon
constexpr int RSSI_THRESHOLD = -55;
// RSSI range used to drive the NeoPixel proximity ring.
// At/below RSSI_RING_MIN: 0 LEDs lit. At/above RSSI_RING_MAX: full ring lit.
// RSSI_RING_MAX matches RSSI_THRESHOLD so the ring fills exactly as the clue advances.
constexpr int RSSI_RING_MIN = -100;
constexpr int RSSI_RING_MAX = -55;

// GLOBALS
// Object to perform BLE scan
BLEScan *pBLEScan;
// Allows progress to be stored in persistent non-volatile memory
Preferences preferences;
// Create e-Paper display object (GDEY042T81 / SSD1683 driver)
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
  display(GxEPD2_420_GDEY042T81(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
// Counter of current progress
int currentClueIndex = 0;
// Whether the hunt has been started via BLE
bool huntStarted = false;
// Advertised BLE characteristic for manual override
BLECharacteristic *currentClueCharacteristic;
// Count of concurrently-connected centrals. We allow more than one so a peer
// prop, or a second operator phone, can connect alongside the controller.
// The Bluedroid stack defaults to 3 simultaneous incoming connections.
volatile uint8_t bleConnections = 0;
// NeoPixel ring for proximity feedback
Adafruit_NeoPixel ring(NUM_LEDS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
// Latest RSSI observed for the current target beacon (INT_MIN = never seen / stale).
int currentTargetRssi = INT_MIN;
// Position of the comet head for the idle "searching" animation (no target signal).
int idleIndex = 0;
// Whether an async BLE scan is currently in progress.
volatile bool scanRunning = false;
// millis() timestamp of the last time the current target beacon was seen.
// Used to decide whether the ring should show the bar graph or the idle animation.
uint32_t lastTargetSeenMs = 0;
// How long after the last detection we keep showing the bar graph before switching
// back to the idle animation. Must exceed the beacon's advertising interval, otherwise
// the ring flickers between bar graph and idle between ads.
constexpr uint32_t TARGET_GRACE_MS = 4000;
// Minimum gap between successive clue advances. Prevents a single "close encounter"
// from skipping past a clue if the next beacon also happens to be within threshold.
constexpr uint32_t ADVANCE_COOLDOWN_MS = 3000;
uint32_t lastAdvanceMs = 0;

// Hunt-complete state: set when the final beacon is found. Drives the finale screen
// and the rainbow victory ring animation.
bool huntComplete = false;

// VoiceRecognizer finale trigger. Once the hunt is complete, the book keeps
// scanning and — when it sees the VoiceRecognizer prop at a close-enough signal —
// connects to it and writes a "play track 10" command so it plays 0010.mp3.
// Fires once per power cycle. The connect happens on the Arduino task (loop()),
// never on the BLE scan callback (a client connect blocks for up to a few seconds).
constexpr char VOICE_DEVICE_NAME[] = "VoiceRecognizer";
constexpr int  VOICE_RSSI_THRESHOLD = -60;  // "reasonable signal" proximity gate
// VoiceRecognizer's Nordic UART Service + its RX characteristic (central -> prop).
static BLEUUID VOICE_NUS_SERVICE("6E400001-B5A3-F393-E0A9-E50E24DCCA9E");
static BLEUUID VOICE_NUS_RX("6E400002-B5A3-F393-E0A9-E50E24DCCA9E");
volatile bool pendingPlayVoice = false;  // set from scan callback, serviced in loop()
bool voicePlayed = false;                // one-shot latch
BLEAddress voiceAddress("00:00:00:00:00:00");  // captured VoiceRecognizer address

// Deferred UI work requested from BLE callbacks. E-paper refresh takes ~4 seconds
// and must not run on the BLE task.
enum PendingDraw : uint8_t { DRAW_NONE, DRAW_START, DRAW_CLUE, DRAW_FINALE };
volatile PendingDraw pendingDraw = DRAW_NONE;
volatile bool pendingStopScan = false;

// DRAWING FUNCTIONS (prototypes — Arduino IDE generates these automatically; PlatformIO needs them)
void drawMultilineCenteredText(const char* text, int startY, int lineHeight);
void drawStartScreen();
void drawClueScreen();
void drawDivider(int16_t y);
void updateProximityRing(int rssi);
void scanCompleteCallback(BLEScanResults results);
void drawFinaleScreen();
void animateVictoryRing();
bool triggerVoiceRecognizer();

// CALLBACKS
/**
 * Callback when new value is assigned to the current clue characteristic
 */
class currentClueCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if(value.length() > 0) {
      // "0" = reset to start screen
      if(value == "0") {
        huntStarted = false;
        huntComplete = false;
        currentClueIndex = 0;
        preferences.putInt("progress", 0);
        Serial.println("Hunt reset via BLE — showing start screen.");
        ring.clear();
        ring.show();
        pendingStopScan = true;
        pendingDraw = DRAW_START;
        return;
      }
      // "1" = start the hunt
      if(value == "1") {
        if(!huntStarted) {
          huntStarted = true;
          Serial.println("Hunt started via BLE!");
          pendingDraw = DRAW_CLUE;
        }
        return;
      }
      // "done" = force hunt completion, exactly as if the final beacon had been
      // found: show the finale screen, run the victory ring, and let loop() trigger
      // the VoiceRecognizer finale sound. Must come before the atoi() fallback below
      // (atoi("done") == 0, which would otherwise jump to the first clue). Clearing
      // the voice latch lets the Oracle sound fire again even after a prior win.
      if(value == "done") {
        huntStarted = true;
        huntComplete = true;
        currentClueIndex = TOTAL_CLUES - 1;
        preferences.putInt("progress", currentClueIndex);
        voicePlayed = false;
        pendingPlayVoice = false;
        currentTargetRssi = INT_MIN;
        lastTargetSeenMs = 0;
        Serial.println("Hunt completed via BLE — showing finale.");
        pendingStopScan = true;
        pendingDraw = DRAW_FINALE;
        return;
      }
      // Other numeric values set the clue index directly (2+)
      int newIndex = atoi(value.c_str());
      if(newIndex >= 0 && newIndex < TOTAL_CLUES) {
        if(!huntStarted) huntStarted = true;
        huntComplete = false;
        currentClueIndex = newIndex;
        preferences.putInt("progress", currentClueIndex);
        pendingDraw = DRAW_CLUE;
        Serial.printf("Clue index manually set to %d\n", currentClueIndex);
      }
    }
  }
};

/**
 * Tracks how many centrals are connected and keeps the prop advertising even
 * after a connection lands. Without restarting advertising on connect, only one
 * central can ever find us — the second phone / peer prop sees an empty scan.
 */
class serverCallback : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    bleConnections++;
    Serial.printf("[BLE] peer connected (now %u)\n", bleConnections);
    BLEDevice::startAdvertising();
  }
  void onDisconnect(BLEServer*) override {
    if (bleConnections > 0) bleConnections--;
    Serial.printf("[BLE] peer disconnected (now %u)\n", bleConnections);
    BLEDevice::startAdvertising();
  }
};

/**
 * Callback when new BLE beacon is in range
 */
class advertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    uint8_t* detectedMAC = (uint8_t*)advertisedDevice.getAddress().getNative();
    int rssi = advertisedDevice.getRSSI();
    if (huntComplete) {
      // After the hunt: watch for the VoiceRecognizer prop and ask it (once) to
      // play the finale sound when the book gets close. Defer the actual connect
      // to loop() — it blocks and must not run on the BLE task.
      if (!voicePlayed && !pendingPlayVoice && rssi >= VOICE_RSSI_THRESHOLD) {
        bool isVoice = (advertisedDevice.haveName() &&
                        advertisedDevice.getName() == VOICE_DEVICE_NAME) ||
                       advertisedDevice.isAdvertisingService(VOICE_NUS_SERVICE);
        if (isVoice) {
          voiceAddress = advertisedDevice.getAddress();
          pendingPlayVoice = true;
          pendingStopScan = true;
          Serial.printf("VoiceRecognizer in range (RSSI %d) — will trigger sound.\n", rssi);
        }
      }
      return;
    }
    if (memcmp(detectedMAC, clues[currentClueIndex].mac, 6) == 0) {
      currentTargetRssi = rssi;
      lastTargetSeenMs = millis();
      // Ring is driven from loop() to avoid fighting with the idle animation.
      if (rssi > RSSI_THRESHOLD && millis() - lastAdvanceMs > ADVANCE_COOLDOWN_MS) {
        Serial.println("Found correct beacon with good signal!");
        lastAdvanceMs = millis();
        currentTargetRssi = INT_MIN;
        lastTargetSeenMs = 0;
        pendingStopScan = true;
        if (currentClueIndex < TOTAL_CLUES - 1) {
          currentClueIndex++;
          preferences.putInt("progress", currentClueIndex);
          pendingDraw = DRAW_CLUE;
        } else {
          huntComplete = true;
          Serial.println("Congratulations! You found the treasure!");
          pendingDraw = DRAW_FINALE;
        }
      }
    }
  }
};

/**
 * Helper function to draw centre-aligned multiline clue text
 */
void drawMultilineCenteredText(const char* text, int startY, int lineHeight) {
  char buffer[256];
  strncpy(buffer, text, sizeof(buffer));
  buffer[sizeof(buffer) - 1] = '\0';
  char* line = strtok(buffer, "\n");
  int y = startY;
  while (line != nullptr) {
    int16_t tbx, tby;
    uint16_t tbw, tbh;
    display.getTextBounds(line, 0, y, &tbx, &tby, &tbw, &tbh);
    int16_t x = (display.width() - tbw) / 2;
    display.setCursor(x, y);
    display.print(line);
    y += lineHeight;
    line = strtok(nullptr, "\n");
  }
}

/**
 * Fullscreen splash/title screen on startup
 */
void drawStartScreen(){
  display.init(115200, true, 2, false);
  display.setRotation(3);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawInvertedBitmap(0, 0, epd_bitmap_title_new, 300, 400, GxEPD_BLACK);
  }
  while (display.nextPage());
  display.hibernate();
}

/**
 * Decorative horizontal divider with a centred diamond flourish, used to
 * separate the clue image (top) from the riddle text (bottom).
 */
void drawDivider(int16_t y){
  const int16_t scrW = display.width();
  const int16_t cx = scrW / 2;
  const int16_t margin = 28;   // gap from the screen edges
  const int16_t gap = 14;      // gap on each side of the centre diamond
  const int16_t d = 7;         // diamond half-size

  // Horizontal rules flanking the diamond
  display.drawLine(margin, y, cx - gap, y, GxEPD_BLACK);
  display.drawLine(cx + gap, y, scrW - margin, y, GxEPD_BLACK);

  // Centre diamond (two filled triangles meeting along the rule)
  display.fillTriangle(cx, y - d, cx - d, y, cx + d, y, GxEPD_BLACK);
  display.fillTriangle(cx, y + d, cx - d, y, cx + d, y, GxEPD_BLACK);

  // Small dots capping the outer ends of the rules
  display.fillCircle(margin, y, 2, GxEPD_BLACK);
  display.fillCircle(scrW - margin, y, 2, GxEPD_BLACK);
}

/**
 * Draw image and text for current clue: image on top, a divider flourish,
 * then the riddle text on the bottom.
 */
void drawClueScreen(){
  display.init(115200, true, 2, false);
  display.setRotation(3);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    uint16_t bmpWidth = clues[currentClueIndex].imageWidth;
    uint16_t bmpHeight = clues[currentClueIndex].imageHeight;
    const int16_t scrW = display.width();

    // Image flush to the top, horizontally centred.
    int16_t x = (scrW - (int16_t)bmpWidth) / 2;
    if (x < 0) x = 0;
    display.drawInvertedBitmap(x, 0, clues[currentClueIndex].imageData, bmpWidth, bmpHeight, GxEPD_BLACK);

    // Flourish separating the art from the riddle.
    int16_t dividerY = (int16_t)bmpHeight + 14;
    drawDivider(dividerY);

    // Riddle text below the divider.
    const GFXfont* currentFont = &FreeMonoBold12pt7b;
    display.setFont(currentFont);
    display.setTextColor(GxEPD_BLACK);
    uint16_t lineHeight = currentFont->yAdvance;
    drawMultilineCenteredText(clues[currentClueIndex].text, dividerY + 38, lineHeight);
  }
  while (display.nextPage());
  display.hibernate();
}

/**
 * Celebratory screen shown once the final beacon is found.
 */
void drawFinaleScreen() {
  display.init(115200, true, 2, false);
  display.setRotation(3);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeMonoBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    uint16_t lineHeight = FreeMonoBold12pt7b.yAdvance;
    drawMultilineCenteredText(
        "Now all whisper\nstones are found,\n"
        "But whispers barely\nmake a sound.\n"
        "To make their\nshy voices complete,\n"
        "Bring the whispers\nto his feet.",
        125, lineHeight);
  } while (display.nextPage());
  display.hibernate();
}

/**
 * Rainbow spin on the ring while hunt is complete.
 * Non-blocking — advances one frame per ~30ms loop tick.
 */
void animateVictoryRing() {
  static uint32_t lastTickMs = 0;
  static uint16_t hueOffset = 0;
  if (millis() - lastTickMs < 30) return;
  lastTickMs = millis();
  hueOffset += 512; // advance the rainbow
  for (int i = 0; i < NUM_LEDS; i++) {
    uint16_t hue = hueOffset + (i * 65536L / NUM_LEDS);
    ring.setPixelColor(i, ring.gamma32(ring.ColorHSV(hue)));
  }
  ring.show();
}

/**
 * Light up the NeoPixel ring as a proximity bar-graph.
 * Number of LEDs lit scales with RSSI of the current target beacon.
 * Color sweeps red (far) -> yellow -> green (close) across lit LEDs.
 */
void updateProximityRing(int rssi) {
  static uint32_t lastIdleTick = 0;
  ring.clear();
  if (rssi > RSSI_RING_MIN) {
    int span = RSSI_RING_MAX - RSSI_RING_MIN;
    int lit = ((rssi - RSSI_RING_MIN) * NUM_LEDS) / span;
    if (lit < 1) lit = 1;
    if (lit > NUM_LEDS) lit = NUM_LEDS;
    for (int i = 0; i < lit; i++) {
      // Color across the whole ring footprint, not just the lit prefix,
      // so a given LED position always means the same proximity level.
      uint8_t r = map(i, 0, NUM_LEDS - 1, 255, 0);
      uint8_t g = map(i, 0, NUM_LEDS - 1, 0, 255);
      ring.setPixelColor(i, ring.Color(r, g, 0));
    }
  } else {
    // No (or too-weak) target signal: a blue "comet" — a bright head with a
    // fading tail — streaks around the ring. The tail trails behind the head
    // (lower indices), opposite the direction of travel.
    if (millis() - lastIdleTick > 110) {
      idleIndex = (idleIndex + 1) % NUM_LEDS;
      lastIdleTick = millis();
    }
    constexpr int COMET_TAIL = 6;  // number of LEDs trailing the head
    for (int k = 0; k <= COMET_TAIL; k++) {
      int pos = (idleIndex - k + NUM_LEDS) % NUM_LEDS;
      // Brightness fades from the head (k=0) to a dim ember at the tail tip.
      float f = (float)(COMET_TAIL - k + 1) / (COMET_TAIL + 1);
      uint8_t blue  = ring.gamma8((uint8_t)(255.0f * f));
      // White-hot head: red/green ride a sharper falloff so only the leading
      // LEDs glow white, fading to a pure blue tail behind them.
      uint8_t white = ring.gamma8((uint8_t)(255.0f * f * f * f));
      ring.setPixelColor(pos, ring.Color(white, white, blue));
    }
  }
  ring.show();
}

/**
 * Fires when an async BLE scan window completes.
 * Clears accumulated results and clears the in-progress flag so loop() can kick off the next scan.
 */
void scanCompleteCallback(BLEScanResults results) {
  pBLEScan->clearResults();
  scanRunning = false;
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== BOOT ===");
  Serial.println(__FILE__ __DATE__);
  Serial.print(TOTAL_CLUES);
  Serial.println(" clues loaded.");

  // Load previous progress first so clue screen shows the right clue
  preferences.begin("TreasureHunt", false);
  currentClueIndex = preferences.getInt("progress", 0);
  Serial.printf("Loaded clue index: %d\n", currentClueIndex);

  // NeoPixel proximity ring
  ring.begin();
  ring.setBrightness(40);
  ring.clear();
  ring.show();

  // Display — drawStartScreen() also calls display.init(), so no need to pre-init here.
  Serial.println("Initialising e-Paper Display");
  SPI.begin(PIN_CLK, -1, PIN_DIN, PIN_CS);
  Serial.println("Drawing start screen...");
  uint32_t t0 = millis();
  drawStartScreen();
  Serial.printf("drawStartScreen took %lums\n", millis() - t0);
  Serial.println("Waiting for BLE 'START' command...");

  // Initialise Bluetooth. The device name is what BLE scanners (Bluefy, the
  // BTEscapeRoomController webapp, nRF Connect) display in their device list,
  // so set it to something recognisable.
  BLEDevice::init("TreasureHunt");

  // Start BLE scanning
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new advertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  // Create BLE characteristic service
  BLEServer *pServer = BLEDevice::createServer();
  pServer->setCallbacks(new serverCallback());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  currentClueCharacteristic = pService->createCharacteristic(
    CLUE_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  currentClueCharacteristic->setCallbacks(new currentClueCallback());
  currentClueCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  // Split-packet advertising so iOS/Bluefy can find us via the webapp:
  //   - Primary packet carries Flags + 128-bit service UUID. CoreBluetooth's
  //     `scanForPeripheralsWithServices` matches only against AD types in the
  //     primary packet, so the UUID must live here for the controller's
  //     `{services: [...]}` filter to work on iOS — iOS doesn't support a
  //     `name:` scan filter at the OS level, which is why a name-only
  //     advertisement makes the prop invisible to a filtered `requestDevice`
  //     on Bluefy (it only shows up under `acceptAllDevices`).
  //   - Scan response carries the complete local name. iOS active-scans on
  //     filtered discovery, picks up the scan response, and merges the name
  //     into `peripheral.name`, so the device still displays as "TreasureHunt".
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();

  BLEAdvertisementData advData;
  advData.setFlags(0x06);  // LE General Discoverable + BR/EDR Not Supported
  advData.setCompleteServices(BLEUUID(SERVICE_UUID));
  pAdvertising->setAdvertisementData(advData);

  BLEAdvertisementData scanResp;
  scanResp.setName("TreasureHunt");
  pAdvertising->setScanResponseData(scanResp);

  pAdvertising->setScanResponse(true);
  pAdvertising->start();

  Serial.println("Setup complete.");
}

/**
 * Connect to the VoiceRecognizer prop and tell it to play the finale track
 * (writes "p10" to its Nordic UART RX characteristic -> plays /mp3/0010.mp3).
 * Returns true on success. Must run on the Arduino task (loop()), never on the
 * BLE scan callback — a client connect blocks for up to a few seconds.
 *
 * The client is created once and reused across retries; the esp32 BLE library
 * has no clean way to free a BLEClient, so we avoid leaking one per attempt.
 */
bool triggerVoiceRecognizer() {
  static BLEClient* voiceClient = nullptr;
  if (voiceClient == nullptr) voiceClient = BLEDevice::createClient();

  Serial.println("Connecting to VoiceRecognizer...");
  if (!voiceClient->connect(voiceAddress)) {
    Serial.println("  connect failed; will retry on next sighting.");
    return false;
  }

  bool ok = false;
  BLERemoteService* svc = voiceClient->getService(VOICE_NUS_SERVICE);
  if (svc != nullptr) {
    BLERemoteCharacteristic* rx = svc->getCharacteristic(VOICE_NUS_RX);
    if (rx != nullptr && rx->canWrite()) {
      rx->writeValue("p10", false);  // play /mp3/0010.mp3 on the prop
      Serial.println("  sent play command (p10).");
      ok = true;
    } else {
      Serial.println("  RX characteristic missing / not writable.");
    }
  } else {
    Serial.println("  NUS service not found on VoiceRecognizer.");
  }
  voiceClient->disconnect();
  return ok;
}

void loop() {
  // Service deferred actions requested from BLE callbacks (scan stop + e-paper draw).
  if (pendingStopScan) {
    pBLEScan->stop();
    scanRunning = false;
    pendingStopScan = false;
  }
  if (pendingDraw != DRAW_NONE) {
    PendingDraw d = pendingDraw;
    pendingDraw = DRAW_NONE;
    switch (d) {
      case DRAW_START:  drawStartScreen();  break;
      case DRAW_CLUE:   drawClueScreen();   break;
      case DRAW_FINALE: drawFinaleScreen(); break;
      default: break;
    }
  }

  // Hunt complete: rainbow victory animation. Keep scanning (just) long enough to
  // find the VoiceRecognizer prop and trigger its finale sound once.
  if (huntComplete) {
    animateVictoryRing();
    if (!voicePlayed) {
      if (pendingPlayVoice) {
        pendingPlayVoice = false;
        if (triggerVoiceRecognizer()) voicePlayed = true;
        // On failure, leave the latch clear; the scan restarts below and retries
        // the next time VoiceRecognizer is seen in range.
      } else if (!scanRunning) {
        scanRunning = true;
        pBLEScan->start(SCAN_DURATION, scanCompleteCallback, false);
      }
    }
    delay(10);
    return;
  }
  // Waiting for BLE start: idle blue dot.
  if (!huntStarted) {
    updateProximityRing(INT_MIN);
    delay(50);
    return;
  }
  // Kick off a fresh async scan if the previous one has ended.
  if (!scanRunning) {
    scanRunning = true;
    pBLEScan->start(SCAN_DURATION, scanCompleteCallback, false);
    Serial.printf("Scanning... Current Clue: %d, RSSI: %d\n",
                  currentClueIndex + 1, currentTargetRssi);
  }
  // Single source of truth for the ring: if the target was seen recently, show
  // the bar graph; otherwise show the idle animation.
  if (millis() - lastTargetSeenMs < TARGET_GRACE_MS) {
    updateProximityRing(currentTargetRssi);
  } else {
    currentTargetRssi = INT_MIN; // let the bar graph "forget" once grace expires
    updateProximityRing(INT_MIN);
  }
  delay(50);
}
