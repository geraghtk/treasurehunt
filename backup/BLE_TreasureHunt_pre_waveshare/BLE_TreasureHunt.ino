/**
* Bluetooth BLE Beacon Treasure Hunt
* Copyright 2025 Alastair Aitchison, Playful Technology
*/

// DEFINES
// Set to 0 to disable Serial pin/BUSY diagnostics.
#ifndef EPD_DEBUG
#define EPD_DEBUG 1
#endif

// 4.2" B/W panel driver (set in platformio.ini: build_flags = -D USE_GDEY042T81=0 or 1)
#ifndef USE_GDEY042T81
#define USE_GDEY042T81 0
#endif
#ifndef EPD_SOLID_TEST
#define EPD_SOLID_TEST 0
#endif

// Waveshare 4.2" SPI on ESP32 (must match display constructor)
static const int EPD_PIN_CS   = 5;
static const int EPD_PIN_DC   = 17;
static const int EPD_PIN_RST  = 16;
static const int EPD_PIN_BUSY = 4;
static const int EPD_PIN_SCK  = 18;
static const int EPD_PIN_MISO = 19;
static const int EPD_PIN_MOSI = 23;

// Advertise a BLE service to enable remote override of current clue
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CLUE_CHAR_UUID      "abcd1234-abcd-1234-abcd-12345678abcd"

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
constexpr int SCAN_DURATION = 5;
// Beacon sticker labels and MAC addresses:
//   Strawberry : f2:6c:8d:69:56:5d
//   Flower     : c5:45:6a:b7:9c:d5
//   Dog        : d8:fe:b6:49:be:57
//   Cat        : d5:65:c6:25:a1:5a
//   Dumbo      : e6:b9:7d:7d:4b:8e
//   Penguin    : d6:e7:1f:86:0b:56
constexpr Clue clues[] = {
    {{0xF2, 0x6C, 0x8D, 0x69, 0x56, 0x5D}, "Your path begins\nat a living arc,\nfrom earth to sky\nthen back to bark.", 300, 207, epd_bitmap_tree},        // Strawberry
    {{0xC5, 0x45, 0x6A, 0xB7, 0x9C, 0xD5}, "Step with care\na tree once stood,\nyet now lies down\nabove the flood.", 300, 156, epd_bitmap_river_trunk},    // Flower
    {{0xD8, 0xFE, 0xB6, 0x49, 0xBE, 0x57}, "Beneath the trees\na gorge runs deep,\na winding trail\nwhere secrets sleep.", 300, 169, epd_bitmap_path},      // Dog
    {{0xD5, 0x65, 0xC6, 0x25, 0xA1, 0x5A}, "An ancient guard\nits heart long gone,\na hollow soul\nto journey on.", 300, 300, epd_bitmap_logo},              // Cat
    {{0xE6, 0xB9, 0x7D, 0x7D, 0x4B, 0x8E}, "Full and round,\nits branches spread,\na bushy crown\nabove your head.", 300, 200, epd_bitmap_logo},             // Dumbo   TODO: replace image
    {{0xD6, 0xE7, 0x1F, 0x86, 0x0B, 0x56}, "Where paws have worn\nthe ground down flat,\na loyal friend\nreturns to that.", 300, 200, epd_bitmap_logo}};    // Penguin TODO: replace image
constexpr int TOTAL_CLUES = sizeof(clues) / sizeof(Clue);
// Threshold of how strong signal needs to be. Range from -90 (weak) to 0 (strong)
// Higher values (i.e. less negative, closer to zero) mean the player will need to get closer to each beacon
constexpr int RSSI_THRESHOLD = -90;

// GLOBALS
// Object to perform BLE scan
BLEScan *pBLEScan;
// Allows progress to be stored in persistent non-volatile memory
Preferences preferences;
// USE_GDEY042T81=1: GDEY042T81 + SSD1683 (Waveshare "V2" GDEY — GxEPD2_420_GDEY042T81)
// USE_GDEY042T81=0: GDEW042T2 + IL0398 (older 4.2" B/W — GxEPD2_420) — original Playful Technology sketch used this
// BUSY=-1: use fixed delays; set to 4 if BUSY polarity matches GxEPD2 for your panel (see GxEPD2 examples)
#if USE_GDEY042T81
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT> display(GxEPD2_420_GDEY042T81(/*CS=*/ 5, /*DC=*/ 17, /*RST=*/ 16, /*BUSY=*/ -1));
#else
GxEPD2_BW<GxEPD2_420, GxEPD2_420::HEIGHT> display(GxEPD2_420(/*CS=*/ 5, /*DC=*/ 17, /*RST=*/ 16, /*BUSY=*/ -1));
#endif
// Counter of current progress
int currentClueIndex = 0;
// Advertised BLE characteristic for manual override
BLECharacteristic *currentClueCharacteristic;

// GxEPD2 toggles CS on GPIO5 in software. Do NOT pass CS as the SPI peripheral's SS pin — that double-drives CS and can clock garbage (blank screen). Use SS=-1 (manual CS only).
static void epdSpiAfterInit() {
  SPI.begin(EPD_PIN_SCK, EPD_PIN_MISO, EPD_PIN_MOSI, -1);
  pinMode(EPD_PIN_CS, OUTPUT);
  digitalWrite(EPD_PIN_CS, HIGH);
}

#if EPD_DEBUG
// Log control + SPI + BUSY lines (BUSY is still physical GPIO4 even when GxEPD2 uses BUSY=-1).
void epdDebugDumpPins(const char* tag) {
  Serial.printf("[EPD] %s: CS=%d DC=%d RST=%d BUSY=%d | SPI SCK=%d MISO=%d MOSI=%d\n",
                tag,
                digitalRead(EPD_PIN_CS), digitalRead(EPD_PIN_DC), digitalRead(EPD_PIN_RST), digitalRead(EPD_PIN_BUSY),
                digitalRead(EPD_PIN_SCK), digitalRead(EPD_PIN_MISO), digitalRead(EPD_PIN_MOSI));
}

static volatile uint32_t s_epd_busy_edges = 0;

void IRAM_ATTR epdBusyIsr() {
  s_epd_busy_edges++;
}

void epdDebugBusyMonitorBegin() {
  pinMode(EPD_PIN_BUSY, INPUT);
  s_epd_busy_edges = 0;
  attachInterrupt(digitalPinToInterrupt(EPD_PIN_BUSY), epdBusyIsr, CHANGE);
}

void epdDebugBusyMonitorEnd() {
  detachInterrupt(digitalPinToInterrupt(EPD_PIN_BUSY));
  Serial.printf("[EPD] BUSY GPIO%d transitions during draw: %u (expect >0 if BUSY wire works)\n",
                EPD_PIN_BUSY, (unsigned)s_epd_busy_edges);
  epdDebugDumpPins("after draw");
}
#endif

// DRAWING FUNCTIONS (prototypes — Arduino IDE generates these automatically; PlatformIO needs them)
void drawMultilineCenteredText(const char* text, int startY, int lineHeight);
void drawStartScreen();
void drawClueScreen();

// CALLBACKS
/**
 * Callback when new value is assigned to the current clue characteristic
 */
class currentClueCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if(value.length() > 0) {
      int newIndex = atoi(value.c_str());
      if(newIndex >= 0 && newIndex < TOTAL_CLUES) {
        currentClueIndex = newIndex;
        preferences.putInt("progress", currentClueIndex);
        drawClueScreen();
        Serial.printf("Clue index manually set to %d\n", currentClueIndex);
      }
    }
  }
};

/**
 * Callback when new BLE beacon is in range
 */
class advertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertisedDevice) override {
    uint8_t* detectedMAC = (uint8_t*)advertisedDevice.getAddress().getNative();
    int rssi = advertisedDevice.getRSSI();
    if(rssi > RSSI_THRESHOLD) { // Beacon is close enough
      if (memcmp(detectedMAC, clues[currentClueIndex].mac, 6) == 0 && rssi > RSSI_THRESHOLD) {
        Serial.println("Found correct beacon with good signal!");
        // If we haven't yet reached the end of the hunt
        if (currentClueIndex < TOTAL_CLUES - 1) {
          currentClueIndex++;
          preferences.putInt("progress", currentClueIndex);
          drawClueScreen();
        }
        else {
          Serial.println("Congratulations!\nYou found the treasure!");
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
  Serial.println("  [drawStartScreen] init...");
#if EPD_DEBUG
  epdDebugDumpPins("drawStartScreen before init");
#endif
  display.init(115200, true, 2, false);
  epdSpiAfterInit();
#if USE_GDEY042T81
  display.epd2.selectFastFullUpdate(false); // slower full refresh; more reliable on some SSD1683 panels
#endif
  display.setRotation(3); // portrait: 300 x 400 for GDEY042T81
  display.setFullWindow();
  constexpr int16_t TITLE_W = 300;
  constexpr int16_t TITLE_H = 400;
  Serial.println("  [drawStartScreen] firstPage...");
#if EPD_DEBUG
  epdDebugBusyMonitorBegin();
#endif
  display.firstPage();
  do {
#if EPD_SOLID_TEST
    display.fillScreen(GxEPD_BLACK); // diagnostic: entire screen black
#else
    display.fillScreen(GxEPD_WHITE);
    display.drawInvertedBitmap(0, 0, epd_bitmap_title, TITLE_W, TITLE_H, GxEPD_BLACK);
#endif
  }
  while (display.nextPage());
#if EPD_DEBUG
  epdDebugBusyMonitorEnd();
#endif
  Serial.println("  [drawStartScreen] done, hibernating.");
  display.hibernate();
}

/**
 * Draw image and text for current clue
 */
void drawClueScreen(){
  // If display has previously been put into hibernation, need to re-initialise it
  display.init(115200, true, 2, false);
  epdSpiAfterInit();
#if USE_GDEY042T81
  display.epd2.selectFastFullUpdate(false);
#endif
  display.setRotation(3); // Set portrait (this setting is not saved when hibernating)
  display.setFullWindow();
#if EPD_DEBUG
  epdDebugBusyMonitorBegin();
#endif
  display.firstPage();
  do {
    // 1.) Clear screen
    display.fillScreen(GxEPD_WHITE);

    // 2.) Draw Bitmap (anchored to bottom; x/w multiples of 8 not required for GFX draw)
    uint16_t bmpWidth = clues[currentClueIndex].imageWidth;
    uint16_t bmpHeight = clues[currentClueIndex].imageHeight;
    const int16_t scrW = display.width();
    const int16_t scrH = display.height();
    int16_t x = (scrW - (int16_t)bmpWidth) / 2;
    if (x < 0) x = 0;
    int16_t y = scrH - (int16_t)bmpHeight;
    if (y < 0) y = 0;
    display.drawInvertedBitmap(x, y, clues[currentClueIndex].imageData, bmpWidth, bmpHeight, GxEPD_BLACK);

    // 3.) Draw Text
    const GFXfont* currentFont = &FreeMonoBold12pt7b;
    display.setFont(currentFont);
    display.setTextColor(GxEPD_BLACK);
    uint16_t lineHeight = currentFont->yAdvance;
    drawMultilineCenteredText(clues[currentClueIndex].text, 50, lineHeight);
  }
  while (display.nextPage());
#if EPD_DEBUG
  epdDebugBusyMonitorEnd();
#endif
  display.hibernate();
}

void setup() {
  Serial.begin(115200);
  delay(5000); // wait for serial monitor to connect
  Serial.println("=== BOOT ===");
  Serial.println(__FILE__ __DATE__);
  Serial.print(TOTAL_CLUES);
  Serial.println(" clues loaded.");

  // VSPI: SCK/MISO/MOSI only; CS is manual on GPIO5 (see epdSpiAfterInit)
  SPI.begin(EPD_PIN_SCK, EPD_PIN_MISO, EPD_PIN_MOSI, -1);
  pinMode(EPD_PIN_CS, OUTPUT);
  digitalWrite(EPD_PIN_CS, HIGH);
  Serial.println("Calling display.init...");
  uint32_t t0 = millis();
  display.init(115200, true, 2, false); // 2ms reset: Waveshare boards with "clever" reset circuit (per GxEPD2 examples)
  epdSpiAfterInit();
#if USE_GDEY042T81
  display.epd2.selectFastFullUpdate(false);
#endif
  Serial.printf("display.init took %lums\n", millis() - t0);
#if EPD_DEBUG
  epdDebugDumpPins("after setup display.init");
  Serial.println("[EPD] If CS/RST idle high and BUSY toggles during draws, wiring is plausible.");
#endif
#if EPD_SOLID_TEST
  Serial.println("[EPD] EPD_SOLID_TEST: title draw is solid black only (set EPD_SOLID_TEST=0 after test).");
#endif
  display.setRotation(3);
  Serial.println("Drawing start screen...");
  t0 = millis();
  drawStartScreen();
  Serial.printf("drawStartScreen took %lums\n", millis() - t0);
  Serial.println("Waiting 10s...");
  delay(10000);

  // Load previous progress
  preferences.begin("TreasureHunt", false);
  currentClueIndex = preferences.getInt("progress", 0);
  Serial.printf("Loaded clue index: %d\n", currentClueIndex);

  Serial.println("Drawing clue screen...");
  t0 = millis();
  drawClueScreen();
  Serial.printf("drawClueScreen took %lums\n", millis() - t0);
  // drawClueScreen() already hibernates
  Serial.println("Setup complete.");

  // Initialise Bluetooth
  BLEDevice::init("");

  // Start BLE scanning
  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new advertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

  // Create BLE characteristic service
  BLEServer *pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService(SERVICE_UUID);
  currentClueCharacteristic = pService->createCharacteristic(
    CLUE_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  currentClueCharacteristic->setCallbacks(new currentClueCallback());
  currentClueCharacteristic->addDescriptor(new BLE2902());
  pService->start();
  // Advertise BLE service
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
}

void loop() {
  pBLEScan->start(SCAN_DURATION, false);
  Serial.printf("Scanning... Current Clue: %d\n", currentClueIndex + 1);
}
