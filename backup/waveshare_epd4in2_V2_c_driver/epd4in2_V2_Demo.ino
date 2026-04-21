/**
 * Waveshare 4.2" e-Paper B/W V2 (monochrome) — Arduino/epd4in2_V2
 * https://github.com/waveshareteam/e-Paper/tree/master/Arduino/epd4in2_V2
 * Pins: epdif.h (ESP32: CLK18 DIN23 CS5 DC21 RST22 BUSY4)
 */
#include <Arduino.h>
#include <SPI.h>
#include "epd4in2_V2.h"
#include "imagedata.h"

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("epd4in2_V2 B/W demo — starting");
  Serial.flush();

  Epd epd;

  if (epd.Init() != 0) {
    Serial.println("e-Paper init failed");
    return;
  }

  epd.Clear();
  delay(500);
  Serial.println("Full update: butterfly");
  epd.Display(IMAGE_BUTTERFLY);
  delay(2000);
  epd.Sleep();
}

void loop() {}
