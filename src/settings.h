// Persistent settings (stored in the ESP32's NVS flash) and the helpers the tools use to apply them.
#pragma once
#include <Arduino.h>

#define SL_VERSION "2.0"

struct Config {
  uint8_t brightness = 140;   // backlight 16..255
  uint8_t dimIdx = 4;         // index into DIM_SEC: dim after 2 minutes
  uint8_t offIdx = 0;         // index into OFF_SEC: never switch the screen off
  uint8_t micGain = 8;        // ES8311 PGA steps of 3 dB (0..10)
  uint8_t volume = 200;       // default speaker volume 0..255
};

extern Config cfg;
void cfgLoad();
void cfgSave();
uint32_t cfgDimMs();          // 0 = never
uint32_t cfgOffMs();          // 0 = never
