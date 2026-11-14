// SensorLab for M5Stack Cardputer ADV - hardware exploration toolkit.
// Keys everywhere:  ; up   . down   , left   / right   ENTER select   ` or Backspace or G0 = back
#include "apps.h"
#include "settings.h"
#include "ui.h"

#include "menu_draw.h"

// Same order as MENU_ITEMS in menu_draw.h
static void (*const RUN[])() = {appWifiScan, appWifiMonitor, appBle, appHunt, appMic, appPlayer, appImu, appSd,
                                appEditor, appIr, appBattery, appChip, appPins, appI2c, appKeys, appScreen, appSettings};
static const int NITEMS = MENU_COUNT;

void setup() {
  auto m5cfg = M5.config();
  m5cfg.internal_imu = true;
  m5cfg.internal_mic = true;
  m5cfg.internal_spk = true;
  M5Cardputer.begin(m5cfg, true);
  M5Cardputer.Display.setRotation(1);
  cfgLoad();
  M5Cardputer.Display.setBrightness(cfg.brightness);
  ui::begin();
  ui::sdMount();
  analogSetPinAttenuation(10, ADC_11db);
  // Board detection can leave the expansion-header pins driven as outputs. SensorLab does not use
  // them, so park them as plain inputs: nothing plugged into the EXT header gets driven against.
  for (int pin : {3, 4, 5, 6, 13, 15}) pinMode(pin, INPUT);
  pinMode(0, INPUT_PULLUP);

  ui::clear();
  cv.setTextColor(C_ACC); cv.setTextSize(3); cv.setCursor(30, 36); cv.print("SensorLab");
  cv.setTextSize(1); cv.setTextColor(C_DIM); cv.setCursor(58, 72); cv.print("Cardputer ADV toolkit");
  cv.setCursor(40, 90);
  cv.setTextColor(M5.getBoard() == m5::board_t::board_M5CardputerADV ? C_OK : C_WARN);
  cv.print(M5.getBoard() == m5::board_t::board_M5CardputerADV ? "   Cardputer ADV detected" : "Warning: ADV not detected");
  ui::push();
  delay(900);
}

void loop() {
  static int sel = 0, top = 0;
  static bool dirty = true;
  static uint32_t lastDraw = 0;
  ui::Key k = ui::poll();
  if (k.up) { sel = (sel + NITEMS - 1) % NITEMS; dirty = true; }
  if (k.down) { sel = (sel + 1) % NITEMS; dirty = true; }
  if (k.left) { sel = max(0, sel - MENU_ROWS); dirty = true; }
  if (k.right) { sel = min(NITEMS - 1, sel + MENU_ROWS); dirty = true; }
  if (k.ch >= '1' && k.ch <= '9') { sel = k.ch - '1'; dirty = true; }
  if (k.ch == '0') { sel = 9; dirty = true; }
  if (k.enter) {
    RUN[sel]();
    dirty = true;
  }
  if (millis() - lastDraw > 5000) dirty = true;      // refresh battery in the header
  if (!dirty) { delay(15); return; }
  dirty = false;
  lastDraw = millis();
  // keep the selection in the middle row where possible
  top = sel - 1;
  if (top > NITEMS - MENU_ROWS) top = NITEMS - MENU_ROWS;
  if (top < 0) top = 0;
  ui::clear();
  ui::header("SensorLab");
  drawMenuBody(cv, sel, top);
  ui::footer(";/. move   ,// page   ENTER open");
  ui::push();
}
