// Shared UI layer: off-screen canvas, keyboard events, SD helpers, text pager.
#pragma once
#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <functional>
#include <vector>

extern M5Canvas cv;

// Screen geometry (240 x 135, 6x8 font -> 40 columns)
static const int SCR_W = 240, SCR_H = 135;
static const int TOP = 12;          // first content pixel row
static const int BOTTOM = 124;      // last content pixel row + 1
static const int LINE_H = 9;
static const int ROWS = (BOTTOM - TOP) / LINE_H;  // 12 text rows

// Palette
static const uint16_t C_BG = 0x0000, C_FG = 0xFFFF, C_DIM = 0x7BEF, C_ACC = 0x05FF,
                      C_HDR = 0x18E3, C_OK = 0x07E0, C_WARN = 0xFD20, C_BAD = 0xF800,
                      C_SEL = 0x2945, C_YEL = 0xFFE0, C_MAG = 0xF81F;

namespace ui {

struct Key {
  bool any = false;
  char ch = 0;                 // printable character, lower-cased copy in lc
  char lc = 0;
  bool enter = false, del = false, tab = false, space = false;
  bool fn = false, ctrl = false, opt = false, alt = false, shift = false;
  bool up = false, down = false, left = false, right = false;
  bool back = false;           // ` (esc), backspace or the G0 button
  bool g0 = false;
};

void begin();
Key poll();                    // call once per loop; also runs M5Cardputer.update()
void header(const char* title, const char* right = nullptr);
void footer(const char* hint);
void clear();
void push();
void toast(const String& msg, uint16_t color = C_OK, uint32_t ms = 1000);
bool confirm(const String& question);
// Text entry box. Type to edit, Backspace deletes, ENTER accepts, G0 or Fn+` cancels.
bool input(const char* title, String& value, int maxLen = 40);
// Small pop-up list. Returns the chosen index or -1 when cancelled.
int popup(const char* title, const char* const* items, int count);
void setRepeatAll(bool on);
void injectKey(char c);         // queue a key press: a character, or '\n' enter, '\b' backspace, '\t' tab, 27 = G0/back
void stayAwake();              // call from screens people watch without pressing keys    // auto-repeat every key (text editing) instead of navigation keys only

// SD card
bool sdMount();                // mounts on first use, returns false when no card
bool sdReady();
String nextFile(const char* prefix, const char* ext);   // /SensorLab/<prefix>_NNNN.<ext>
bool saveLines(const char* prefix, const std::vector<String>& lines);

// Scrollable "label<TAB>value" list. Lines starting with '#' are section titles.
// fill() is called every refreshMs (0 = once). S saves the lines to the SD card.
void pager(const char* title, std::function<void(std::vector<String>&)> fill,
           uint32_t refreshMs = 0, const char* savePrefix = nullptr);

String hex(const uint8_t* d, size_t n, char sep = 0);
String mac(const uint8_t* m);
int battPct();                 // same estimate the Battery screen uses
float battVolts();             // averaged cell voltage (GPIO10, 1:2 divider)
int lipoPct(float volts);
}  // namespace ui
