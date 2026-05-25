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
    {{0xE4, 0xCE, 0x26, 0x90, 0x96, 0xCF}, "Step with care\na tree once stood,\nyet now lies down\nabove the flood.", 300, 156, epd_bitmap_river_trunk},      // Flower
    {{0xF2, 0x6C, 0x8D, 0x69, 0x56, 0x5D}, "Your path begins\nat a living arc,\nfrom earth to sky\nthen back to bark.", 300, 207, epd_bitmap_tree},        // Strawberry
    {{0xD8, 0xFE, 0xB6, 0x49, 0xBE, 0x57}, "Beneath the trees\na gorge runs deep,\na winding trail\nwhere secrets sleep.", 300, 169, epd_bitmap_path},      // Dog
    {{0xD5, 0x65, 0xC6, 0x25, 0xA1, 0x5A}, "An ancient guard\nits heart long gone,\na hollow soul\nto journey on.", 300, 300, epd_bitmap_logo},              // Cat
    {{0xE6, 0xB9, 0x7D, 0x7D, 0x4B, 0x8E}, "Full and round,\nits branches spread,\na bushy crown\nabove your head.", 300, 200, epd_bitmap_logo},             // Dumbo   TODO: replace image
    {{0xD6, 0xE7, 0x1F, 0x86, 0x0B, 0x56}, "Where paws have worn\nthe ground down flat,\na loyal friend\nreturns to that.", 300, 200, epd_bitmap_logo}};    // Penguin TODO: replace image
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
// Position of the rotating "searching" pixel when no target signal is detected.
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

// Deferred UI work requested from BLE callbacks. E-paper refresh takes ~4 seconds
// and must not run on the BLE task.
enum PendingDraw : uint8_t { DRAW_NONE, DRAW_START, DRAW_CLUE, DRAW_FINALE };
volatile PendingDraw pendingDraw = DRAW_NONE;
volatile bool pendingStopScan = false;

// DRAWING FUNCTIONS (prototypes — Arduino IDE generates these automatically; PlatformIO needs them)
void drawMultilineCenteredText(const char* text, int startY, int lineHeight);
void drawStartScreen();
void drawClueScreen();
void updateProximityRing(int rssi);
void scanCompleteCallback(BLEScanResults results);
void drawFinaleScreen();
void animateVictoryRing();

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
    if (huntComplete) return;
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
 * Draw image and text for current clue
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
    const int16_t scrH = display.height();
    int16_t x = (scrW - (int16_t)bmpWidth) / 2;
    if (x < 0) x = 0;
    int16_t y = scrH - (int16_t)bmpHeight;
    if (y < 0) y = 0;
    display.drawInvertedBitmap(x, y, clues[currentClueIndex].imageData, bmpWidth, bmpHeight, GxEPD_BLACK);

    const GFXfont* currentFont = &FreeMonoBold12pt7b;
    display.setFont(currentFont);
    display.setTextColor(GxEPD_BLACK);
    uint16_t lineHeight = currentFont->yAdvance;
    drawMultilineCenteredText(clues[currentClueIndex].text, 50, lineHeight);
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
    drawMultilineCenteredText("You found\nthe treasure!\n\nCongratulations!", 120, lineHeight);
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
    // No (or too-weak) target signal: rotate a dim blue "searching" pixel.
    if (millis() - lastIdleTick > 150) {
      idleIndex = (idleIndex + 1) % NUM_LEDS;
      lastIdleTick = millis();
    }
    ring.setPixelColor(idleIndex, ring.Color(0, 0, 40));
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

  // Hunt complete: rainbow victory animation, no more scanning.
  if (huntComplete) {
    animateVictoryRing();
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
