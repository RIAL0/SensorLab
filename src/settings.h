// Persistent settings (stored in the ESP32's NVS flash) and the helpers the tools use to apply them.
#pragma once
#include <Arduino.h>

struct Config {
  uint8_t brightness = 140;   // backlight 16..255
  uint8_t dimIdx = 4;         // index into DIM_SEC: dim after 2 minutes
  uint8_t offIdx = 0;         // index into OFF_SEC: never switch the screen off
  bool ledOn = false;         // RGB LED on the Stamp module (GPIO21)
  uint8_t ledColor = 1;       // index into the colour table
  uint8_t ledLevel = 0;       // 0 low, 1 medium, 2 high
  uint8_t micGain = 8;        // ES8311 PGA steps of 3 dB (0..10)
  uint8_t volume = 200;       // default speaker volume 0..255
};

extern Config cfg;
void cfgLoad();
void cfgSave();
void cfgApplyLed();           // sets the LED to the configured state
uint32_t cfgDimMs();          // 0 = never
uint32_t cfgOffMs();          // 0 = never
