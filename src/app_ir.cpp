// IR transmitter test (IR LED on GPIO44) and the Stamp RGB LED (GPIO21).
#include "apps.h"
#include "settings.h"
#include "ui.h"

#define DISABLE_CODE_FOR_RECEIVER
#define SEND_PWM_BY_TIMER
#define IR_TX_PIN 44
#include <IRremote.hpp>

void appIr() {
  IrSender.begin(DISABLE_LED_FEEDBACK);
  IrSender.setSendPin(IR_TX_PIN);
  static const char* protos[] = {"NEC", "Samsung", "Sony 12-bit", "RC5"};
  int proto = 0, field = 0, led = 0;
  uint16_t addr = 0x00, cmd = 0x45;
  uint32_t sent = 0, flashUntil = 0;
  static const uint8_t ledCols[][3] = {{0, 0, 0}, {40, 0, 0}, {0, 40, 0}, {0, 0, 40}, {40, 40, 40}};
  static const char* ledNames[] = {"off", "red", "green", "blue", "white"};
  bool dirty = true;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.any) dirty = true;
    if (k.lc == 'p') proto = (proto + 1) % 4;
    if (k.left || k.right || k.tab) field ^= 1;
    if (k.up || k.down) {
      int step = k.up ? 1 : -1;
      if (field == 0) addr = (addr + step) & 0xFF; else cmd = (cmd + step) & 0xFF;
    }
    if (k.lc == 'l') {
      led = (led + 1) % 5;
      ledSet(ledCols[led][0], ledCols[led][1], ledCols[led][2]);
    }
    if (k.enter || k.space) {
      switch (proto) {
        case 0: IrSender.sendNEC(addr, cmd, 0); break;
        case 1: IrSender.sendSamsung(addr, cmd, 0); break;
        case 2: IrSender.sendSony(addr & 0x1F, cmd & 0x7F, 2, 12); break;
        case 3: IrSender.sendRC5(addr & 0x1F, cmd & 0x3F, 0, true); break;
      }
      sent++;
      flashUntil = millis() + 250;
    }
    if (flashUntil && millis() > flashUntil) { flashUntil = 0; dirty = true; }
    if (!dirty) { delay(15); continue; }
    dirty = false;
    ui::clear();
    ui::header("IR + LED Test");
    int y = TOP + 4;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Protocol");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(80, y); cv.print(protos[proto]);
    y += 14;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Address");
    cv.setTextColor(field == 0 ? C_YEL : C_FG, field == 0 ? C_SEL : C_BG); cv.setCursor(80, y); cv.printf(" 0x%02X (%3d) ", addr, addr);
    y += 12;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Command");
    cv.setTextColor(field == 1 ? C_YEL : C_FG, field == 1 ? C_SEL : C_BG); cv.setCursor(80, y); cv.printf(" 0x%02X (%3d) ", cmd, cmd);
    y += 14;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Sent");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(80, y); cv.printf("%lu frames  38 kHz  GPIO44", (unsigned long)sent);
    y += 12;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("RGB LED");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(80, y); cv.printf("%s  (GPIO21)", ledNames[led]);
    if (flashUntil) { cv.fillCircle(214, TOP + 20, 10, C_BAD); cv.setTextColor(C_FG); cv.setCursor(205, TOP + 17); cv.print("TX"); }
    else cv.drawCircle(214, TOP + 20, 10, C_SEL);
    y += 16;
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y); cv.print("Tip: a phone camera shows the IR LED");
    cv.setCursor(4, y + 9); cv.print("flashing violet when a frame is sent.");
    ui::footer("ENTER send P proto ,// ;/. edit L led");
    ui::push();
  }
  cfgApplyLed();                 // back to the LED state chosen in Settings
}
