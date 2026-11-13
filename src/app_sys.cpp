// Battery, GPIO pin map, and ESP32-S3 chip information.
#include "apps.h"
#include "ui.h"
#include <WiFi.h>
#include <esp_chip_info.h>
#include <esp_efuse.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>
#include <soc/gpio_periph.h>
#include <soc/rtc.h>
#include <soc/usb_serial_jtag_reg.h>

// ------------------------------------------------------------------ battery
// The ADV has no fuel-gauge chip: the cell voltage reaches GPIO10 through a 1:2 divider.
// So voltage is measured; percentage, trend and time estimates are derived from it.
static int lipoPct(float v) { return ui::lipoPct(v); }
static float readBattV() { return ui::battVolts(); }

// True while a USB host (computer, phone, powered hub) is talking to the ESP32-S3: the host sends
// a start-of-frame every millisecond and the chip counts them. A plain wall charger sends none.
static bool usbHostPresent() {
  static uint32_t lastFrame = 0xFFFF, lastAt = 0;
  static bool present = false;
  uint32_t now = millis();
  if (now - lastAt >= 100) {
    uint32_t f = REG_READ(USB_SERIAL_JTAG_FRAM_NUM_REG) & 0x7FF;
    present = (lastFrame != 0xFFFF) && (f != lastFrame);
    lastFrame = f;
    lastAt = now;
  }
  return present;
}

static void drawBolt(int x, int y, uint16_t col) {      // 10 x 18 lightning bolt
  cv.fillTriangle(x + 7, y, x, y + 10, x + 5, y + 10, col);
  cv.fillTriangle(x + 3, y + 18, x + 10, y + 8, x + 5, y + 8, col);
}

enum ChargeState { CH_DETECT, CH_CHARGING, CH_FULL, CH_BATTERY, CH_NOCELL };

void appBattery() {
  const int HIST = 190, WIN = 120;            // graph: 5 s/point (16 min); trend window: 120 x 0.5 s = 60 s
  static float hist[HIST], win[WIN];
  int hn = 0, wn = 0, wpos = 0;
  float vMin = 9, vMax = 0, v = readBattV(), vFilt = v, vFast = v;
  uint32_t t0 = millis(), sampleAt = 0, histAt = 0, lastDraw = 0;
  float vStart = v, slope60 = 0;
  ChargeState st = CH_DETECT;
  bool byUsb = false, latched = false;
  bool logging = false;
  File logf;
  String logName;
  usbHostPresent();

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.lc == 's') {
      if (logging) { logging = false; logf.close(); ui::toast("Saved " + logName.substring(11)); }
      else if (!ui::sdMount()) ui::toast("No SD card", C_BAD);
      else {
        logName = ui::nextFile("batt", "csv");
        logf = SD.open(logName, FILE_WRITE);
        if (logf) { logf.println("seconds,volts,percent,state"); logging = true; }
        else ui::toast("SD write failed", C_BAD);
      }
    }
    bool usb = usbHostPresent();
    if (millis() - sampleAt >= 500) {
      sampleAt = millis();
      v = readBattV();
      vFilt = vFilt * 0.8f + v * 0.2f;
      vFast = vFast * 0.5f + v * 0.5f;
      if (vFilt < vMin) vMin = vFilt;
      if (vFilt > vMax) vMax = vFilt;
      // 60 s ring of the smoothed voltage
      float old8 = wn >= 8 ? win[(wpos + WIN - 8) % WIN] : vFilt;       // value 4 s ago
      float oldest = wn >= WIN ? win[wpos] : (wn ? win[0] : vFilt);
      float spanMin = (wn >= WIN ? WIN : wn) * 0.5f / 60.0f;
      win[wpos] = vFilt;
      wpos = (wpos + 1) % WIN;
      if (wn < WIN) wn++;
      slope60 = spanMin > 0.25f ? (vFilt - oldest) * 1000.0f / spanMin : 0;

      // ---- charge state. There is no charge-status signal on this board, so it is inferred:
      //  1. USB data link present            -> external power for certain
      //  2. sudden voltage step up / down    -> charger plugged / unplugged
      //  3. steady rise / fall over a minute -> charging / discharging
      float step = vFast - old8;
      if (vFilt < 2.5f) { st = CH_NOCELL; latched = false; }
      else if (usb) { st = vFilt >= 4.16f && slope60 < 1.0f ? CH_FULL : CH_CHARGING; byUsb = true; latched = true; }
      else {
        if (byUsb) { byUsb = false; latched = false; st = CH_DETECT; }     // host went away
        if (wn >= 8 && step > 0.035f) { st = CH_CHARGING; latched = true; }
        else if (wn >= 8 && step < -0.035f) { st = CH_BATTERY; latched = true; }
        else if (spanMin >= 0.5f) {
          if (slope60 > 1.0f) st = CH_CHARGING;
          else if (slope60 < -0.5f) st = CH_BATTERY;
          else if (vFilt >= 4.17f) st = CH_FULL;
          else if (!latched && st == CH_DETECT && spanMin >= 1.0f) st = CH_BATTERY;
        }
      }
    }
    if (millis() - histAt >= 5000 || hn == 0) {       // one graph point every 5 s (16 min window)
      histAt = millis();
      if (hn < HIST) hist[hn++] = vFilt;
      else { memmove(hist, hist + 1, (HIST - 1) * sizeof(float)); hist[HIST - 1] = vFilt; }
      if (logging) {
        logf.printf("%lu,%.3f,%d,%s\n", (unsigned long)((millis() - t0) / 1000), vFilt, lipoPct(vFilt),
                    st == CH_CHARGING ? "charging" : st == CH_FULL ? "full" : st == CH_BATTERY ? "battery" : "unknown");
        logf.flush();
      }
    }
    if (millis() - lastDraw < 250) { delay(10); continue; }
    lastDraw = millis();

    float mins = (millis() - t0) / 60000.0f;
    float slope = mins > 1.0f ? (vFilt - vStart) * 1000.0f / mins : slope60;     // mV per minute
    int pct = lipoPct(vFilt);
    const char* label; const char* how; uint16_t sc;
    switch (st) {
      case CH_CHARGING: label = "CHARGING"; sc = C_OK; break;
      case CH_FULL: label = "FULL - ON POWER"; sc = C_ACC; break;
      case CH_BATTERY: label = "ON BATTERY"; sc = C_WARN; break;
      case CH_NOCELL: label = "NO BATTERY"; sc = C_BAD; break;
      default: label = "DETECTING..."; sc = C_DIM; break;
    }
    how = st == CH_NOCELL ? "no cell / switch off" : byUsb ? "USB data link detected" : st == CH_DETECT ? "watching voltage ~30 s" : "inferred from voltage";
    bool blink = (millis() / 500) & 1;

    ui::clear();
    ui::header("Battery", logging ? "LOG" : nullptr);
    // gauge
    cv.drawRect(4, TOP + 2, 60, 26, C_FG);
    cv.fillRect(64, TOP + 10, 4, 10, C_FG);
    cv.fillRect(6, TOP + 4, 56 * pct / 100, 22, pct > 50 ? C_OK : pct > 20 ? C_YEL : C_BAD);
    cv.setTextColor(C_FG); cv.setTextSize(2); cv.setCursor(pct >= 100 ? 8 : 14, TOP + 8); cv.printf("%d%%", pct); cv.setTextSize(1);
    // charge status badge
    int bx = 74, bw = SCR_W - bx - 2;
    cv.fillRoundRect(bx, TOP + 2, bw, 26, 4, st == CH_DETECT ? C_HDR : C_SEL);
    cv.drawRoundRect(bx, TOP + 2, bw, 26, 4, sc);
    if (st == CH_CHARGING) { if (blink) drawBolt(bx + 6, TOP + 6, C_YEL); }
    else if (st == CH_FULL) drawBolt(bx + 6, TOP + 6, C_ACC);
    else if (st == CH_BATTERY) { cv.drawRect(bx + 5, TOP + 10, 12, 9, sc); cv.fillRect(bx + 17, TOP + 12, 2, 5, sc); cv.fillRect(bx + 7, TOP + 12, 5, 5, sc); }
    else cv.drawCircle(bx + 11, TOP + 15, 5, sc);
    cv.setTextColor(sc); cv.setCursor(bx + 24, TOP + 6); cv.print(label);
    cv.setTextColor(C_DIM); cv.setCursor(bx + 24, TOP + 17); cv.printf("%.22s", how);

    cv.setTextColor(C_FG, C_BG); cv.setCursor(4, TOP + 32); cv.printf("%.3f V", vFilt);
    cv.setTextColor(C_DIM, C_BG); cv.printf("  min %.3f  max %.3f", vMin, vMax);
    cv.setCursor(4, TOP + 41);
    cv.printf("trend %+.1f mV/min", slope60);
    if (st == CH_BATTERY && mins > 3.0f && slope < -0.2f) {
      float left = (vFilt - 3.30f) * 1000.0f / -slope;
      cv.printf("  ~%dh%02dm left", (int)(left / 60), (int)left % 60);
    } else if (st == CH_CHARGING && mins > 3.0f && slope > 0.5f && vFilt < 4.15f) {
      float left = (4.18f - vFilt) * 1000.0f / slope;
      cv.printf("  ~%dh%02dm to full", (int)(left / 60), (int)left % 60);
    } else {
      cv.printf("  USB host %s", usb ? "yes" : "no");
    }
    // history graph
    const int gy = TOP + 52, gh = 48;
    cv.drawRect(4, gy, HIST + 4, gh, C_SEL);
    float lo = 9, hi = 0;
    for (int i = 0; i < hn; i++) { if (hist[i] < lo) lo = hist[i]; if (hist[i] > hi) hi = hist[i]; }
    if (hi - lo < 0.05f) { float m = (hi + lo) / 2; lo = m - 0.025f; hi = m + 0.025f; }
    int py = 0;
    for (int i = 0; i < hn; i++) {
      int y = gy + gh - 2 - (int)((hist[i] - lo) / (hi - lo) * (gh - 4));
      if (i) cv.drawLine(5 + i, py, 6 + i, y, sc == C_DIM ? C_ACC : sc); else cv.drawPixel(6, y, C_ACC);
      py = y;
    }
    int lx = HIST + 12;
    cv.setTextColor(C_FG, C_BG); cv.setCursor(lx, gy + 1); cv.printf("%.3f", hi);
    cv.setCursor(lx, gy + gh - 8); cv.printf("%.3f", lo);
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(lx, gy + gh / 2 - 8); cv.print("volts");
    cv.setCursor(lx, gy + gh / 2 + 1); cv.print("5s/pt");
    ui::footer(logging ? "S stop log   ` back" : "S log to CSV   ` back");
    ui::push();
  }
  if (logging) logf.close();
}

// ------------------------------------------------------------------ pins
struct PinInfo { int8_t gpio; char group; const char* use; };
// group: I = used inside the Cardputer ADV, X = brought out to a connector,
//        F = no known function on this board, S = system (flash/USB/strap), N = does not exist
static const PinInfo PINS[] = {
  {0, 'I', "G0 button (BtnA) / boot strap"},
  {1, 'X', "Grove port SCL / G1 (ADC1)"},
  {2, 'X', "Grove port SDA / G2 (ADC1)"},
  {3, 'X', "EXT header (LoRa cap RST) strap"},
  {4, 'X', "EXT header (LoRa cap IRQ)"},
  {5, 'X', "EXT header (LoRa cap NSS/CS)"},
  {6, 'X', "EXT header (LoRa cap BUSY)"},
  {7, 'F', "not used"},
  {8, 'I', "I2C SDA: keyboard, IMU, codec"},
  {9, 'I', "I2C SCL: keyboard, IMU, codec"},
  {10, 'I', "Battery voltage ADC (1:2)"},
  {11, 'I', "Keyboard TCA8418 interrupt"},
  {12, 'I', "microSD CS"},
  {13, 'X', "EXT header (GPS cap UART)"},
  {14, 'I', "microSD MOSI (also EXT hdr)"},
  {15, 'X', "EXT header (GPS cap UART)"},
  {16, 'F', "not used"},
  {17, 'F', "not used"},
  {18, 'F', "not used"},
  {19, 'S', "USB D-"},
  {20, 'S', "USB D+"},
  {21, 'I', "RGB LED (WS2812) on Stamp"},
  {22, 'N', "does not exist on ESP32-S3"},
  {23, 'N', "does not exist on ESP32-S3"},
  {24, 'N', "does not exist on ESP32-S3"},
  {25, 'N', "does not exist on ESP32-S3"},
  {26, 'S', "SPI flash CS1 (reserved)"},
  {27, 'S', "SPI flash HD"},
  {28, 'S', "SPI flash WP"},
  {29, 'S', "SPI flash CS0"},
  {30, 'S', "SPI flash CLK"},
  {31, 'S', "SPI flash Q"},
  {32, 'S', "SPI flash D"},
  {33, 'I', "Display reset"},
  {34, 'I', "Display DC (data/command)"},
  {35, 'I', "Display MOSI"},
  {36, 'I', "Display SCLK"},
  {37, 'I', "Display CS"},
  {38, 'I', "Display backlight PWM"},
  {39, 'I', "microSD MISO (also EXT hdr)"},
  {40, 'I', "microSD SCK (also EXT hdr)"},
  {41, 'I', "I2S BCLK (codec ES8311)"},
  {42, 'I', "I2S DOUT -> speaker amp"},
  {43, 'I', "I2S LRCK/WS (codec)"},
  {44, 'I', "IR transmitter LED"},
  {45, 'F', "not used (strap pin)"},
  {46, 'I', "I2S DIN <- microphone (strap)"},
  {47, 'F', "not used"},
  {48, 'F', "not used"},
};
static const int NPINS = sizeof(PINS) / sizeof(PINS[0]);

static bool pinOpenDrain(int n) { return REG_READ(GPIO_PIN0_REG + 4 * n) & (1 << 2); }

static void pinState(int n, bool& level, bool& oe, bool& ie, bool& pu, bool& pd) {
  level = n < 32 ? (REG_READ(GPIO_IN_REG) >> n) & 1 : (REG_READ(GPIO_IN1_REG) >> (n - 32)) & 1;
  oe = n < 32 ? (REG_READ(GPIO_ENABLE_REG) >> n) & 1 : (REG_READ(GPIO_ENABLE1_REG) >> (n - 32)) & 1;
  uint32_t mux = GPIO_PIN_MUX_REG[n] ? REG_READ(GPIO_PIN_MUX_REG[n]) : 0;
  ie = mux & FUN_IE; pu = mux & FUN_PU; pd = mux & FUN_PD;
}

void appPins() {
  int sel = 0, top = 0, filter = 0;
  uint32_t lastDraw = 0;
  std::vector<int> idx;
  static const char* fname[] = {"all", "exposed", "internal", "unused"};
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.up && sel > 0) sel--;
    if (k.down) sel++;
    if (k.left) sel -= ROWS - 2;
    if (k.right) sel += ROWS - 2;
    if (k.lc == 'f' || k.tab) { filter = (filter + 1) % 4; sel = 0; }
    if (k.lc == 's') {
      std::vector<String> L;
      L.push_back("GPIO,group,function,level,output_enabled,input_enabled,pullup,pulldown");
      for (int i = 0; i < NPINS; i++) {
        bool lv = 0, oe = 0, ie = 0, pu = 0, pd = 0;
        if (PINS[i].group != 'N') pinState(PINS[i].gpio, lv, oe, ie, pu, pd);
        char b[120];
        snprintf(b, sizeof(b), "%d,%c,%s,%d,%d,%d,%d,%d", PINS[i].gpio, PINS[i].group, PINS[i].use, lv, oe, ie, pu, pd);
        L.push_back(b);
      }
      ui::saveLines("pins", L);
    }
    if (millis() - lastDraw < 120) { delay(5); continue; }
    lastDraw = millis();
    idx.clear();
    for (int i = 0; i < NPINS; i++) {
      char g = PINS[i].group;
      if (filter == 0 || (filter == 1 && g == 'X') || (filter == 2 && g == 'I') || (filter == 3 && g == 'F')) idx.push_back(i);
    }
    const int rows = ROWS - 2;
    if (sel >= (int)idx.size()) sel = (int)idx.size() - 1;
    if (sel < 0) sel = 0;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;

    ui::clear();
    char right[24];
    snprintf(right, sizeof(right), "%s %d", fname[filter], (int)idx.size());
    ui::header("GPIO Pins", right);
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, TOP); cv.print("IO  function                    lvl dir");
    for (int r = 0; r < rows && top + r < (int)idx.size(); r++) {
      const PinInfo& p = PINS[idx[top + r]];
      int y = TOP + (r + 1) * LINE_H;
      uint16_t bg = (top + r == sel) ? C_SEL : C_BG;
      if (top + r == sel) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
      uint16_t col = p.group == 'X' ? C_OK : p.group == 'I' ? C_ACC : p.group == 'F' ? C_YEL : C_DIM;
      cv.setTextColor(col, bg); cv.setCursor(2, y); cv.printf("%2d", p.gpio);
      char u[30]; snprintf(u, sizeof(u), "%-27.27s", p.use);
      cv.setTextColor(p.group == 'N' ? C_DIM : C_FG, bg); cv.setCursor(24, y); cv.print(u);
      if (p.group != 'N') {
        bool lv, oe, ie, pu, pd;
        pinState(p.gpio, lv, oe, ie, pu, pd);
        cv.setTextColor(lv ? C_OK : C_DIM, bg); cv.setCursor(194, y); cv.print(ie || oe ? (lv ? "HI" : "lo") : "--");
        bool od = oe && pinOpenDrain(p.gpio);
        cv.setTextColor(od ? C_ACC : oe ? C_WARN : C_DIM, bg); cv.setCursor(214, y); cv.print(od ? "OD" : oe ? "OUT" : ie ? "IN" : "off");
      }
    }
    // detail line for the selected pin
    if (!idx.empty()) {
      const PinInfo& p = PINS[idx[sel]];
      int n = p.gpio, y = TOP + (rows + 1) * LINE_H;
      cv.drawFastHLine(0, y - 2, SCR_W, C_SEL);
      cv.setTextColor(C_YEL, C_BG); cv.setCursor(2, y);
      if (p.group == 'N') cv.print("no such pad on this chip");
      else {
        bool lv, oe, ie, pu, pd;
        pinState(n, lv, oe, ie, pu, pd);
        cv.printf("G%d %s%s%s%s%s%s%s", n, oe && pinOpenDrain(n) ? "open-drain " : "", pu ? "pull-up " : "", pd ? "pull-dn " : "", (n >= 1 && n <= 10) ? "ADC1 " : (n >= 11 && n <= 20) ? "ADC2 " : "",
                  (n >= 1 && n <= 14) ? "touch " : "", n <= 21 ? "RTC " : "", (n == 0 || n == 3 || n == 45 || n == 46) ? "STRAP" : "");
      }
    }
    ui::footer("F filter  S save  green=exposed  OD=bus");
    ui::push();
  }
}

// ------------------------------------------------------------------ chip info
static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "crash / panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "wake from deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    default: return "unknown";
  }
}
static const char* flashMode(FlashMode_t m) {
  switch (m) { case FM_QIO: return "QIO"; case FM_QOUT: return "QOUT"; case FM_DIO: return "DIO"; case FM_DOUT: return "DOUT"; default: return "other"; }
}
static const char* boardName() {
  switch (M5.getBoard()) {
    case m5::board_t::board_M5CardputerADV: return "M5Cardputer ADV";
    case m5::board_t::board_M5Cardputer: return "M5Cardputer (original)";
    default: return "other / not detected";
  }
}

void appChip() {
  ui::pager("ESP32-S3 Chip Info", [](std::vector<String>& L) {
    char b[96];
    uint8_t m[6];
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    L.push_back("#Board");
    L.push_back(String("Detected\t") + boardName());
    L.push_back("Firmware\tSensorLab 1.7  " __DATE__);
    snprintf(b, sizeof(b), "Uptime\t%lu s", (unsigned long)(millis() / 1000)); L.push_back(b);
    L.push_back(String("Last reset\t") + resetName(esp_reset_reason()));
    L.push_back("#CPU");
    snprintf(b, sizeof(b), "Model\t%s rev v%d.%d", ESP.getChipModel(), ci.full_revision / 100, ci.full_revision % 100); L.push_back(b);
    snprintf(b, sizeof(b), "Cores\t%d x Xtensa LX7", ci.cores); L.push_back(b);
    snprintf(b, sizeof(b), "CPU clock\t%lu MHz", (unsigned long)getCpuFrequencyMhz()); L.push_back(b);
    snprintf(b, sizeof(b), "Crystal\t%lu MHz", (unsigned long)getXtalFrequencyMhz()); L.push_back(b);
    snprintf(b, sizeof(b), "APB clock\t%lu MHz", (unsigned long)(getApbFrequency() / 1000000)); L.push_back(b);
    snprintf(b, sizeof(b), "Die temp\t%.1f C", temperatureRead()); L.push_back(b);
    String feat;
    if (ci.features & CHIP_FEATURE_WIFI_BGN) feat += "WiFi-bgn ";
    if (ci.features & CHIP_FEATURE_BLE) feat += "BLE ";
    if (ci.features & CHIP_FEATURE_BT) feat += "BT-classic ";
    if (ci.features & CHIP_FEATURE_IEEE802154) feat += "802.15.4 ";
    L.push_back("Radios\t" + feat);
    L.push_back(String("Embedded flash\t") + ((ci.features & CHIP_FEATURE_EMB_FLASH) ? "yes" : "no (external)"));
    L.push_back(String("Embedded PSRAM\t") + ((ci.features & CHIP_FEATURE_EMB_PSRAM) ? "yes" : "no"));
    snprintf(b, sizeof(b), "Package ver\t%lu", (unsigned long)esp_efuse_get_pkg_ver()); L.push_back(b);
    L.push_back("#Memory (RAM)");
    snprintf(b, sizeof(b), "Heap total\t%lu bytes", (unsigned long)ESP.getHeapSize()); L.push_back(b);
    snprintf(b, sizeof(b), "Heap free\t%lu bytes", (unsigned long)ESP.getFreeHeap()); L.push_back(b);
    snprintf(b, sizeof(b), "Lowest free\t%lu bytes", (unsigned long)ESP.getMinFreeHeap()); L.push_back(b);
    snprintf(b, sizeof(b), "Largest block\t%lu bytes", (unsigned long)ESP.getMaxAllocHeap()); L.push_back(b);
    snprintf(b, sizeof(b), "PSRAM\t%lu bytes", (unsigned long)ESP.getPsramSize()); L.push_back(b);
    L.push_back("#Flash");
    uint32_t fid = 0;
    esp_flash_read_id(NULL, &fid);
    snprintf(b, sizeof(b), "Size\t%lu MB", (unsigned long)(ESP.getFlashChipSize() / 1048576)); L.push_back(b);
    snprintf(b, sizeof(b), "JEDEC ID\t0x%06lX", (unsigned long)fid); L.push_back(b);
    snprintf(b, sizeof(b), "Speed / mode\t%lu MHz %s", (unsigned long)(ESP.getFlashChipSpeed() / 1000000), flashMode(ESP.getFlashChipMode())); L.push_back(b);
    snprintf(b, sizeof(b), "This app\t%lu bytes", (unsigned long)ESP.getSketchSize()); L.push_back(b);
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (run) {
      snprintf(b, sizeof(b), "App slot\t%lu bytes (%lu%% used)", (unsigned long)run->size, (unsigned long)(ESP.getSketchSize() * 100ULL / run->size)); L.push_back(b);
      snprintf(b, sizeof(b), "Running from\t%s @0x%lX", run->label, (unsigned long)run->address); L.push_back(b);
    }
    L.push_back("#Partition table");
    for (int t = 0; t < 2; t++) {
      esp_partition_iterator_t it = esp_partition_find(t == 0 ? ESP_PARTITION_TYPE_APP : ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
      while (it) {
        const esp_partition_t* p = esp_partition_get(it);
        snprintf(b, sizeof(b), "%-9.9s\t%s 0x%06lX %luK", p->label, t == 0 ? "app " : "data", (unsigned long)p->address, (unsigned long)(p->size / 1024));
        L.push_back(b);
        it = esp_partition_next(it);
      }
    }
    L.push_back("#MAC addresses");
    esp_efuse_mac_get_default(m); L.push_back("Base (eFuse)\t" + ui::mac(m));
    esp_read_mac(m, ESP_MAC_WIFI_STA); L.push_back("WiFi station\t" + ui::mac(m));
    esp_read_mac(m, ESP_MAC_WIFI_SOFTAP); L.push_back("WiFi AP\t" + ui::mac(m));
    esp_read_mac(m, ESP_MAC_BT); L.push_back("Bluetooth\t" + ui::mac(m));
    L.push_back("#Software");
    L.push_back(String("ESP-IDF\t") + ESP.getSdkVersion());
    snprintf(b, sizeof(b), "Arduino core\t%d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH); L.push_back(b);
    L.push_back("App MD5\t" + ESP.getSketchMD5().substring(0, 16));
    L.push_back(" \t" + ESP.getSketchMD5().substring(16));
  }, 2000, "chip");
}
