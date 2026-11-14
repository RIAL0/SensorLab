// Settings: screen, idle behaviour, LED, audio defaults, USB drive mode, housekeeping.
#include "apps.h"
#include "settings.h"
#include "ui.h"
#include <Preferences.h>
#include "USB.h"
#include "USBMSC.h"

Config cfg;
static Preferences s_prefs;

static const uint16_t DIM_SEC[] = {0, 15, 30, 60, 120, 300};
static const char* const DIM_TXT[] = {"never", "15 s", "30 s", "1 min", "2 min", "5 min"};
static const uint16_t OFF_SEC[] = {0, 60, 120, 300, 600, 1800};
static const char* const OFF_TXT[] = {"never", "1 min", "2 min", "5 min", "10 min", "30 min"};
struct LedCol { const char* name; uint8_t r, g, b; };
static const LedCol LED[] = {{"white", 255, 255, 255}, {"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255},
                             {"yellow", 255, 180, 0}, {"cyan", 0, 255, 255}, {"magenta", 255, 0, 255}, {"orange", 255, 80, 0}};
static const int NLED = sizeof(LED) / sizeof(LED[0]);
static const uint8_t LED_LEVEL[] = {10, 45, 150};
static const char* const LEVEL_TXT[] = {"low", "medium", "high"};

void cfgLoad() {
  s_prefs.begin("slcfg", false);
  Config d;
  cfg.brightness = s_prefs.getUChar("bright", d.brightness);
  cfg.dimIdx = min((int)s_prefs.getUChar("dim", d.dimIdx), 5);
  cfg.offIdx = min((int)s_prefs.getUChar("off", d.offIdx), 5);
  cfg.ledOn = s_prefs.getBool("led", d.ledOn);
  cfg.ledColor = min((int)s_prefs.getUChar("ledcol", d.ledColor), NLED - 1);
  cfg.ledLevel = min((int)s_prefs.getUChar("ledlvl", d.ledLevel), 2);
  cfg.micGain = min((int)s_prefs.getUChar("mic", d.micGain), 10);
  cfg.volume = s_prefs.getUChar("vol", d.volume);
  if (cfg.brightness < 16) cfg.brightness = 16;
}

void cfgSave() {
  s_prefs.putUChar("bright", cfg.brightness);
  s_prefs.putUChar("dim", cfg.dimIdx);
  s_prefs.putUChar("off", cfg.offIdx);
  s_prefs.putBool("led", cfg.ledOn);
  s_prefs.putUChar("ledcol", cfg.ledColor);
  s_prefs.putUChar("ledlvl", cfg.ledLevel);
  s_prefs.putUChar("mic", cfg.micGain);
  s_prefs.putUChar("vol", cfg.volume);
}

// The M5 library owns the RGB LED and redraws it from its own buffer on every update, so the
// colour has to be set through M5.Led; writing to the pin directly is overwritten at once.
void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  if (M5.Led.isEnabled()) {
    M5.Led.setBrightness(255);
    M5.Led.setAllColor(r, g, b);
    M5.Led.display();
  } else {
    neopixelWrite(21, r, g, b);
  }
}

void cfgApplyLed() {
  if (!cfg.ledOn) { ledSet(0, 0, 0); return; }
  const LedCol& c = LED[cfg.ledColor];
  uint8_t l = LED_LEVEL[cfg.ledLevel];
  ledSet(c.r * l / 255, c.g * l / 255, c.b * l / 255);
}

uint32_t cfgDimMs() { return DIM_SEC[cfg.dimIdx] * 1000UL; }
uint32_t cfgOffMs() { return OFF_SEC[cfg.offIdx] * 1000UL; }

// ------------------------------------------------------------------ USB drive mode
// Presents the microSD card to a computer as a USB drive. The computer talks to the card sector by
// sector, so SensorLab must not touch the filesystem meanwhile: the mode ends with a restart.
static USBMSC s_msc;
static volatile uint32_t s_rdSec = 0, s_wrSec = 0, s_ioErr = 0;
static volatile bool s_ejected = false;

static int32_t mscRead(uint32_t lba, uint32_t offset, void* buf, uint32_t size) {
  (void)offset;
  uint32_t n = size / 512;
  for (uint32_t i = 0; i < n; i++)
    if (!SD.readRAW((uint8_t*)buf + i * 512, lba + i)) { s_ioErr = s_ioErr + 1; return -1; }
  s_rdSec = s_rdSec + n;
  return size;
}
static int32_t mscWrite(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t size) {
  (void)offset;
  uint32_t n = size / 512;
  for (uint32_t i = 0; i < n; i++)
    if (!SD.writeRAW(buf + i * 512, lba + i)) { s_ioErr = s_ioErr + 1; return -1; }
  s_wrSec = s_wrSec + n;
  return size;
}
static bool mscStartStop(uint8_t, bool start, bool eject) {
  if (eject && !start) s_ejected = true;
  return true;
}

static void usbDrive() {
  if (!ui::sdMount()) { ui::toast("No SD card", C_BAD); return; }
  ui::clear(); ui::header("USB drive mode");
  cv.setTextColor(C_FG, C_BG);
  cv.setCursor(4, TOP + 4); cv.print("The SD card will appear on the");
  cv.setCursor(4, TOP + 14); cv.print("computer as a USB drive.");
  cv.setTextColor(C_WARN, C_BG);
  cv.setCursor(4, TOP + 32); cv.print("Leaving this mode restarts the");
  cv.setCursor(4, TOP + 42); cv.print("Cardputer. Transfers are slow");
  cv.setCursor(4, TOP + 52); cv.print("(card is on SPI): small files only.");
  cv.setTextColor(C_DIM, C_BG);
  cv.setCursor(4, TOP + 70); cv.print("Use a USB data cable to a computer.");
  ui::footer("ENTER start    any other key cancel"); ui::push();
  while (true) { ui::Key k = ui::poll(); if (k.enter) break; if (k.any) return; delay(15); }

  uint32_t sectors = SD.numSectors();
  s_rdSec = s_wrSec = s_ioErr = 0; s_ejected = false;
  s_msc.vendorID("SensrLab");
  s_msc.productID("Cardputer SD");
  s_msc.productRevision("1.0");
  s_msc.onRead(mscRead);
  s_msc.onWrite(mscWrite);
  s_msc.onStartStop(mscStartStop);
  s_msc.mediaPresent(true);
  s_msc.begin(sectors, 512);
  USB.begin();

  uint32_t last = 0;
  while (true) {
    ui::Key k = ui::poll();
    ui::stayAwake();
    if (k.enter || k.back) {
      bool busy = !s_ejected && (s_wrSec > 0);
      if (!busy || ui::confirm("Not ejected yet. Restart anyway?")) break;
    }
    if (millis() - last < 250) { delay(10); continue; }
    last = millis();
    ui::clear(); ui::header("USB drive mode", s_ejected ? "EJECTED" : "ACTIVE");
    cv.setTextColor(C_OK, C_BG); cv.setCursor(4, TOP + 4);
    cv.printf("Card: %.1f GB as a USB drive", sectors / 2097152.0);
    cv.setTextColor(C_FG, C_BG);
    cv.setCursor(4, TOP + 22); cv.printf("Read     %8.1f MB", s_rdSec / 2048.0);
    cv.setCursor(4, TOP + 32); cv.printf("Written  %8.1f MB", s_wrSec / 2048.0);
    cv.setTextColor(s_ioErr ? C_BAD : C_DIM, C_BG);
    cv.setCursor(4, TOP + 42); cv.printf("Errors   %8lu", (unsigned long)s_ioErr);
    cv.setTextColor(C_WARN, C_BG);
    cv.setCursor(4, TOP + 62); cv.print(s_ejected ? "Ejected: safe to leave." : "Eject the drive on the computer");
    cv.setCursor(4, TOP + 72); cv.print(s_ejected ? "" : "before leaving this mode.");
    cv.setTextColor(C_DIM, C_BG);
    cv.setCursor(4, TOP + 90); cv.print("If no drive appears, check the cable.");
    ui::footer("ENTER finish and restart");
    ui::push();
  }
  ui::toast("Restarting...", C_WARN, 600);
  ESP.restart();
}

// ------------------------------------------------------------------ housekeeping
// Files SensorLab generated: <prefix>_NNNN.<ext>. Notes and ble_names.csv / oui.csv never match.
static bool isGenerated(const String& n, bool recordings) {
  int dot = n.lastIndexOf('.');
  if (dot < 6 || n[dot - 5] != '_') return false;
  for (int i = dot - 4; i < dot; i++) if (!isDigit(n[i])) return false;
  String ext = n.substring(dot + 1); ext.toLowerCase();
  if (n.startsWith("note_")) return false;
  if (recordings) return n.startsWith("rec_") && ext == "wav";
  return ext == "csv" || ext == "txt";
}

static void deleteGenerated(bool recordings) {
  if (!ui::sdMount()) { ui::toast("No SD card", C_BAD); return; }
  std::vector<String> names;
  uint64_t bytes = 0;
  File d = SD.open("/SensorLab");
  while (d) {
    File f = d.openNextFile();
    if (!f) break;
    String n = f.name();
    n = n.substring(n.lastIndexOf('/') + 1);
    if (!f.isDirectory() && isGenerated(n, recordings)) { names.push_back(n); bytes += f.size(); }
    f.close();
  }
  if (d) d.close();
  if (names.empty()) { ui::toast(recordings ? "No recordings to delete" : "No log files to delete", C_DIM, 1100); return; }
  char q[48];
  snprintf(q, sizeof(q), "Delete %d %s (%.1f MB)?", (int)names.size(), recordings ? "recordings" : "log files", bytes / 1048576.0);
  if (!ui::confirm(q)) return;
  int ok = 0;
  for (auto& n : names) if (SD.remove("/SensorLab/" + n)) ok++;
  ui::toast("Deleted " + String(ok) + " of " + String((int)names.size()), ok == (int)names.size() ? C_OK : C_WARN, 1200);
}

// ------------------------------------------------------------------ the menu
enum { R_BRIGHT, R_DIM, R_OFF, R_LED, R_LEDCOL, R_LEDLVL, R_MIC, R_VOL, R_USB, R_DELLOG, R_DELREC, R_RESET, R_ABOUT, R_COUNT };
static const char* const LABEL[] = {"Screen brightness", "Dim when idle", "Screen off when idle", "LED", "LED colour", "LED brightness",
                                    "Mic gain (default)", "Sound volume", "USB drive mode (SD card)", "Delete log files",
                                    "Delete recordings", "Reset settings", "About"};

void appSettings() {
  int sel = 0, top = 0;
  bool dirty = true, changed = false;
  auto& spk = M5Cardputer.Speaker;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (k.up) sel = (sel + R_COUNT - 1) % R_COUNT;
    if (k.down) sel = (sel + 1) % R_COUNT;
    int step = k.right ? 1 : k.left ? -1 : 0;
    if (step || k.enter) {
      int s = step ? step : 1;                    // ENTER steps forward on value rows
      switch (sel) {
        case R_BRIGHT: cfg.brightness = constrain(cfg.brightness + s * 16, 16, 255); M5Cardputer.Display.setBrightness(cfg.brightness); changed = true; break;
        case R_DIM: cfg.dimIdx = (cfg.dimIdx + 6 + s) % 6; changed = true; break;
        case R_OFF: cfg.offIdx = (cfg.offIdx + 6 + s) % 6; changed = true; break;
        case R_LED: cfg.ledOn = !cfg.ledOn; cfgApplyLed(); changed = true; break;
        case R_LEDCOL: cfg.ledColor = (cfg.ledColor + NLED + s) % NLED; cfg.ledOn = true; cfgApplyLed(); changed = true; break;
        case R_LEDLVL: cfg.ledLevel = (cfg.ledLevel + 3 + s) % 3; cfg.ledOn = true; cfgApplyLed(); changed = true; break;
        case R_MIC: cfg.micGain = constrain(cfg.micGain + s, 0, 10); changed = true; break;
        case R_VOL:
          cfg.volume = constrain(cfg.volume + s * 15, 0, 255); changed = true;
          spk.begin(); spk.setVolume(cfg.volume); spk.tone(880, 60);      // let the new level be heard
          break;
        case R_USB: if (k.enter) usbDrive(); break;
        case R_DELLOG: if (k.enter) deleteGenerated(false); break;
        case R_DELREC: if (k.enter) deleteGenerated(true); break;
        case R_RESET:
          if (k.enter && ui::confirm("Reset all settings to defaults?")) {
            cfg = Config(); changed = true;
            M5Cardputer.Display.setBrightness(cfg.brightness); cfgApplyLed();
            ui::toast("Settings reset");
          }
          break;
        case R_ABOUT:
          if (k.enter) ui::pager("About", [](std::vector<String>& L) {
            L.push_back("#SensorLab");
            L.push_back("Version\t1.9");
            L.push_back("Built\t" __DATE__);
            L.push_back("For\tM5Stack Cardputer ADV");
            L.push_back("Licence\tMIT");
            L.push_back("Source\tgithub.com/RIAL0/SensorLab");
            L.push_back("#Where things are kept");
            L.push_back("Settings\tin the ESP32's flash (NVS)");
            L.push_back("Your data\t/SensorLab on the SD card");
            L.push_back("BLE tags\t/SensorLab/ble_names.csv");
          });
          break;
      }
    }
    if (!dirty) { delay(15); continue; }
    dirty = false;
    const int rows = ROWS;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    ui::clear();
    ui::header("Settings");
    for (int r = 0; r < rows && top + r < R_COUNT; r++) {
      int i = top + r, y = TOP + r * LINE_H;
      bool on = i == sel;
      uint16_t bg = on ? C_SEL : C_BG;
      if (on) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
      bool action = i >= R_USB;
      cv.setTextColor(on ? C_FG : action ? C_ACC : C_DIM, bg); cv.setCursor(4, y); cv.print(LABEL[i]);
      char v[24] = "";
      uint16_t vc = C_YEL;
      switch (i) {
        case R_BRIGHT: snprintf(v, sizeof(v), "%d%%", cfg.brightness * 100 / 255); break;
        case R_DIM: snprintf(v, sizeof(v), "%s", DIM_TXT[cfg.dimIdx]); break;
        case R_OFF: snprintf(v, sizeof(v), "%s", OFF_TXT[cfg.offIdx]); break;
        case R_LED: snprintf(v, sizeof(v), "%s", cfg.ledOn ? "on" : "off"); vc = cfg.ledOn ? C_OK : C_DIM; break;
        case R_LEDCOL: snprintf(v, sizeof(v), "%s", LED[cfg.ledColor].name); vc = cv.color565(LED[cfg.ledColor].r, LED[cfg.ledColor].g, LED[cfg.ledColor].b); break;
        case R_LEDLVL: snprintf(v, sizeof(v), "%s", LEVEL_TXT[cfg.ledLevel]); break;
        case R_MIC: snprintf(v, sizeof(v), "+%d dB", cfg.micGain * 3); break;
        case R_VOL: snprintf(v, sizeof(v), "%d%%", cfg.volume * 100 / 255); break;
        default: snprintf(v, sizeof(v), "ENTER"); vc = C_DIM; break;
      }
      cv.setTextColor(vc, bg); cv.setCursor(SCR_W - 4 - (int)strlen(v) * 6, y); cv.print(v);
    }
    ui::footer(sel >= R_USB ? ";/. select   ENTER run   ` back" : ";/. select   ,// change   ` back");
    ui::push();
  }
  if (changed) cfgSave();
}
