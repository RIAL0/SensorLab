// Small tools: I2C bus scanner, keyboard/speaker test, screen test.
#include "apps.h"
#include "ui.h"

// ------------------------------------------------------------------ I2C scanner
static const char* i2cName(uint8_t a, bool internal) {
  if (internal) {
    switch (a) {
      case 0x18: return "ES8311 audio codec";
      case 0x34: return "TCA8418 keyboard controller";
      case 0x68: case 0x69: return "BMI270 IMU (accel + gyro)";
    }
  }
  switch (a) {
    case 0x18: case 0x19: return "ES8311 / LIS3DH / MCP9808";
    case 0x1E: return "HMC5883 / LIS3MDL magnetometer";
    case 0x20: case 0x21: case 0x22: case 0x23: return "PCF8574 / MCP23017 IO expander";
    case 0x29: return "VL53L0X ToF / TCS34725 colour";
    case 0x38: return "AHT20 / FT6336 touch";
    case 0x39: return "APDS9960 / TSL2561";
    case 0x3C: case 0x3D: return "SSD1306 OLED display";
    case 0x40: return "INA219 / SHT40 / PCA9685";
    case 0x44: case 0x45: return "SHT30 / SHT40 temp+humidity";
    case 0x48: case 0x49: return "ADS1115 ADC / TMP102";
    case 0x51: return "PCF8563 / RTC";
    case 0x52: return "Nunchuk / VL53 ToF";
    case 0x53: return "ADXL345 / LTR390";
    case 0x57: return "MAX30102 / EEPROM";
    case 0x58: return "SGP30 air quality";
    case 0x5A: return "MLX90614 IR temp / MPR121";
    case 0x62: return "SCD40 CO2";
    case 0x68: return "MPU6050/6886 / DS3231 / BMI270";
    case 0x69: return "MPU6886 / BMI270";
    case 0x70: return "PCA9548 mux / HT16K33";
    case 0x76: case 0x77: return "BMP280 / BME280 / BME680";
    default: return "unknown device";
  }
}

void appI2c() {
  ui::pager("I2C Bus Scan", [](std::vector<String>& L) {
    bool found[120];
    char b[64];
    int n = 0;
    L.push_back("#Internal bus  SDA G8 / SCL G9");
    memset(found, 0, sizeof(found));
    M5.In_I2C.scanID(found);
    for (int a = 8; a < 120; a++) if (found[a]) { snprintf(b, sizeof(b), "0x%02X\t%s", a, i2cName(a, true)); L.push_back(b); n++; }
    if (!n) L.push_back("no devices answered");
    L.push_back("#Grove port  SDA G2 / SCL G1");
    n = 0;
    memset(found, 0, sizeof(found));
    M5.Ex_I2C.begin();
    M5.Ex_I2C.scanID(found);
    for (int a = 8; a < 120; a++) if (found[a]) { snprintf(b, sizeof(b), "0x%02X\t%s", a, i2cName(a, false)); L.push_back(b); n++; }
    if (!n) L.push_back("nothing connected");
    M5.Ex_I2C.release();
    L.push_back("#Note");
    L.push_back("Rescans every 3 s. Names for Grove");
    L.push_back("devices are best guesses by address.");
  }, 3000, "i2c");
}

// ------------------------------------------------------------------ keyboard + speaker
void appKeys() {
  M5Cardputer.Speaker.begin();
  int vol = 80;
  bool sound = true;
  M5Cardputer.Speaker.setVolume(vol);
  String typed;
  uint32_t presses = 0;
  bool dirty = true;
  while (true) {
    M5Cardputer.update();
    if (M5.BtnA.wasPressed()) break;                 // G0 is the only way out: every key is under test
    if (M5Cardputer.Keyboard.isChange()) {
      dirty = true;
      if (M5Cardputer.Keyboard.isPressed()) {
        auto& st = M5Cardputer.Keyboard.keysState();
        presses++;
        for (char c : st.word) typed += c;
        if (st.del && typed.length()) typed.remove(typed.length() - 1);
        if (st.enter) typed = "";
        if (typed.length() > 38) typed = typed.substring(typed.length() - 38);
        if (st.ctrl) sound = !sound;
        if (st.opt) { vol = vol >= 240 ? 40 : vol + 40; M5Cardputer.Speaker.setVolume(vol); }
        if (sound) {
          auto& kl = M5Cardputer.Keyboard.keyList();
          if (!kl.empty()) M5Cardputer.Speaker.tone(220.0f * powf(2.0f, (kl[0].x + kl[0].y * 14) / 12.0f), 90);
        }
      }
    }
    if (!dirty) { delay(5); continue; }
    dirty = false;
    auto& st = M5Cardputer.Keyboard.keysState();
    auto& kl = M5Cardputer.Keyboard.keyList();
    ui::clear();
    char right[24];
    snprintf(right, sizeof(right), "%s vol %d", sound ? "sound" : "mute", vol);
    ui::header("Keyboard + Speaker", right);
    // 14 x 4 key matrix
    const int kw = 16, kh = 13, ox = 8, oy = TOP + 3;
    for (int y = 0; y < 4; y++) for (int x = 0; x < 14; x++) {
      bool on = false;
      for (auto& p : kl) if (p.x == x && p.y == y) on = true;
      if (on) cv.fillRect(ox + x * kw, oy + y * kh, kw - 2, kh - 2, C_OK); else cv.drawRect(ox + x * kw, oy + y * kh, kw - 2, kh - 2, C_SEL);
    }
    int y = oy + 4 * kh + 3;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, y); cv.print("mods ");
    struct { const char* n; bool on; } mods[] = {{"Fn", st.fn}, {"Shift", st.shift}, {"Ctrl", st.ctrl}, {"Opt", st.opt}, {"Alt", st.alt}, {"Tab", st.tab}, {"Del", st.del}, {"Ent", st.enter}};
    for (auto& m : mods) { cv.setTextColor(m.on ? C_OK : C_SEL, C_BG); cv.print(m.n); cv.print(' '); }
    y += 10;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, y); cv.print("HID  ");
    cv.setTextColor(C_FG, C_BG);
    for (auto h : st.hid_keys) cv.printf("0x%02X ", h);
    cv.setTextColor(C_DIM, C_BG); cv.printf(" keys down %d  total %lu", (int)kl.size(), (unsigned long)presses);
    y += 10;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, y); cv.print("matrix ");
    cv.setTextColor(C_FG, C_BG);
    for (auto& p : kl) cv.printf("(%d,%d) ", p.x, p.y);
    y += 10;
    cv.setTextColor(C_YEL, C_BG); cv.setCursor(2, y); cv.print(typed); cv.print('_');
    ui::footer("G0 button = exit  Ctrl mute  Opt vol");
    ui::push();
  }
}

// ------------------------------------------------------------------ screen
void appScreen() {
  auto& D = M5Cardputer.Display;
  int mode = 0, bright = D.getBrightness();
  const int NMODE = 8;
  uint32_t fps = 0, frames = 0, fpsAt = millis();
  float ph = 0;
  bool dirty = true;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.right || k.enter || k.space) { mode = (mode + 1) % NMODE; dirty = true; }
    if (k.left) { mode = (mode + NMODE - 1) % NMODE; dirty = true; }
    if (k.up || k.down) { bright = constrain(bright + (k.up ? 16 : -16), 8, 255); D.setBrightness(bright); dirty = true; }
    if (mode != 7 && !dirty) { delay(15); continue; }
    dirty = false;
    if (mode == 0) {
      ui::clear();
      ui::header("Screen Info");
      int y = TOP + 1;
      auto row = [&](const char* l, const String& v) { cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print(l); cv.setTextColor(C_FG, C_BG); cv.setCursor(92, y); cv.print(v); y += 9; };
      row("Panel", "ST7789V2 IPS 1.14\"");
      row("Resolution", String(D.width()) + " x " + String(D.height()) + " px");
      row("Colour depth", String((int)D.getColorDepth()) + " bit (RGB565)");
      row("Rotation", String(D.getRotation()));
      row("Bus", "SPI3 40 MHz, 3-wire");
      row("Pins", "SCK36 MOSI35 DC34");
      row("", "CS37 RST33 BL38");
      row("Backlight", String(bright) + " / 255  (;/. adjust)");
      row("Frame buffer", String(SCR_W * SCR_H * 2) + " bytes in RAM");
      row("Pixels", String(SCR_W * SCR_H));
      ui::footer(",// next test pattern   ` back");
    } else if (mode == 1) {
      static const uint16_t bars[] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
      for (int i = 0; i < 8; i++) cv.fillRect(i * 30, 0, 30, SCR_H, bars[i]);
    } else if (mode == 2) {
      for (int x = 0; x < SCR_W; x++) {
        uint8_t v = x * 255 / (SCR_W - 1);
        cv.drawFastVLine(x, 0, 34, cv.color565(v, 0, 0));
        cv.drawFastVLine(x, 34, 34, cv.color565(0, v, 0));
        cv.drawFastVLine(x, 68, 34, cv.color565(0, 0, v));
        cv.drawFastVLine(x, 102, 33, cv.color565(v, v, v));
      }
    } else if (mode >= 3 && mode <= 5) {
      cv.fillSprite(mode == 3 ? 0xFFFF : mode == 4 ? 0xF800 : 0x0000);      // dead / stuck pixel check
    } else if (mode == 6) {
      cv.fillSprite(0);
      for (int y = 0; y < SCR_H; y += 2) for (int x = (y / 2) & 1; x < SCR_W; x += 2) cv.drawPixel(x, y, 0xFFFF);
      cv.drawRect(0, 0, SCR_W, SCR_H, C_BAD);
      cv.drawLine(0, 0, SCR_W - 1, SCR_H - 1, C_OK);
      cv.drawLine(0, SCR_H - 1, SCR_W - 1, 0, C_OK);
    } else {
      // full-frame animation to measure the real refresh rate
      ph += 0.08f;
      for (int y = 0; y < SCR_H; y += 3)
        for (int x = 0; x < SCR_W; x += 3) {
          float v = sinf(x * 0.05f + ph) + sinf(y * 0.07f - ph * 1.3f) + sinf((x + y) * 0.03f + ph * 0.7f);
          cv.fillRect(x, y, 3, 3, cv.color565(128 + 42 * v, 128 + 42 * sinf(v + ph), 128 - 42 * v));
        }
      frames++;
      if (millis() - fpsAt >= 1000) { fps = frames; frames = 0; fpsAt = millis(); }
      cv.setTextColor(C_FG, C_BG); cv.setCursor(4, 4); cv.printf(" %lu fps full-screen ", (unsigned long)fps);
    }
    ui::push();
  }
}
