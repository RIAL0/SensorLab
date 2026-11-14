#include "ui.h"
#include <Preferences.h>
#include <algorithm>

M5Canvas cv(&M5Cardputer.Display);

namespace ui {

static bool s_sdTried = false, s_sdOk = false;
static Preferences prefs;

void begin() {
  cv.setColorDepth(16);
  cv.createSprite(SCR_W, SCR_H);
  cv.setFont(&fonts::Font0);
  cv.setTextSize(1);
  cv.setTextWrap(false);
  prefs.begin("sensorlab", false);
}

// ---------------------------------------------------------------- keyboard
static Key s_last;            // last emitted event, for auto-repeat
static uint32_t s_repeatAt = 0;
static size_t s_count = 0;
static bool s_repeatAll = false;

void setRepeatAll(bool on) { s_repeatAll = on; }

static Key decode() {
  Key k;
  auto& st = M5Cardputer.Keyboard.keysState();
  k.enter = st.enter; k.del = st.del; k.tab = st.tab; k.space = st.space;
  k.fn = st.fn; k.ctrl = st.ctrl; k.opt = st.opt; k.alt = st.alt; k.shift = st.shift;
  // With several keys held, report the one that was just pressed: the character that is in the
  // current set but was not in the previous one.
  static std::vector<char> prevWord;
  if (!st.word.empty()) {
    k.ch = st.word.back();
    std::vector<char> old = prevWord;
    for (char c : st.word) {
      auto it = std::find(old.begin(), old.end(), c);
      if (it == old.end()) { k.ch = c; break; }
      old.erase(it);
    }
  }
  prevWord.assign(st.word.begin(), st.word.end());
  k.lc = (k.ch >= 'A' && k.ch <= 'Z') ? k.ch + 32 : k.ch;
  k.up = (k.ch == ';' || k.ch == ':');
  k.down = (k.ch == '.' || k.ch == '>');
  k.left = (k.ch == ',' || k.ch == '<');
  k.right = (k.ch == '/' || k.ch == '?');
  k.back = (k.ch == '`' || k.ch == '~' || k.del);
  k.any = k.ch || k.enter || k.del || k.tab || k.space;
  return k;
}

Key poll() {
  M5Cardputer.update();
  Key k;
  if (M5.BtnA.wasPressed()) {
    k.any = k.back = k.g0 = true;
    return k;
  }
  uint32_t now = millis();
  if (M5Cardputer.Keyboard.isChange()) {
    // Emit an event only when a key went down, not when one of several held keys was released.
    size_t count = M5Cardputer.Keyboard.keyList().size();
    bool more = count > s_count;
    s_count = count;
    if (more && M5Cardputer.Keyboard.isPressed()) {
      k = decode();
      s_last = k;
      s_repeatAt = now + 400;
      return k;
    }
    if (!more) decode();                 // refresh the held-key record on release
    if (!count) s_last = Key();
    return k;
  }
  // auto-repeat for navigation keys only
  if (M5Cardputer.Keyboard.isPressed() && s_last.any && now >= s_repeatAt &&
      (s_repeatAll || s_last.up || s_last.down || s_last.left || s_last.right)) {
    s_repeatAt = now + 70;
    return s_last;
  }
  return k;
}

// ---------------------------------------------------------------- drawing
int lipoPct(float v) {
  static const float V[] = {3.30f, 3.50f, 3.60f, 3.65f, 3.70f, 3.75f, 3.80f, 3.87f, 3.95f, 4.05f, 4.15f};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 11; i++) if (v < V[i]) return (i - 1) * 10 + (int)((v - V[i - 1]) / (V[i] - V[i - 1]) * 10);
  return 100;
}

float battVolts() {
  uint32_t sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(10);
  return sum / 16.0f * 2.0f / 1000.0f;
}

int battPct() {
  static uint32_t at = 0;
  static float v = 0;
  if (v == 0) v = battVolts();
  if (millis() - at > 3000) {
    v = v * 0.7f + battVolts() * 0.3f;
    at = millis();
  }
  return lipoPct(v);
}

void clear() { cv.fillSprite(C_BG); }

void header(const char* title, const char* right) {
  cv.fillRect(0, 0, SCR_W, 11, C_HDR);
  cv.setTextColor(C_ACC, C_HDR);
  cv.setCursor(2, 2);
  cv.print(title);
  char buf[24];
  if (right) {
    snprintf(buf, sizeof(buf), "%s", right);
  } else {
    snprintf(buf, sizeof(buf), "%s%d%%", s_sdOk ? "SD " : "", battPct());
  }
  cv.setTextColor(C_FG, C_HDR);
  cv.setCursor(SCR_W - 2 - (int)strlen(buf) * 6, 2);
  cv.print(buf);
}

void footer(const char* hint) {
  cv.fillRect(0, SCR_H - 10, SCR_W, 10, C_HDR);
  cv.setTextColor(C_DIM, C_HDR);
  cv.setCursor(2, SCR_H - 9);
  cv.print(hint);
}

void push() { cv.pushSprite(0, 0); }

void toast(const String& msg, uint16_t color, uint32_t ms) {
  int w = msg.length() * 6 + 12;
  if (w > SCR_W) w = SCR_W;
  int x = (SCR_W - w) / 2, y = 54;
  cv.fillRect(x, y, w, 22, C_BG);
  cv.drawRect(x, y, w, 22, color);
  cv.setTextColor(color, C_BG);
  cv.setCursor(x + 6, y + 7);
  cv.print(msg);
  push();
  uint32_t until = millis() + ms;
  while (millis() < until) { M5Cardputer.update(); delay(10); }
}

bool confirm(const String& question) {
  int w = SCR_W - 20, x = 10, y = 44;
  cv.fillRect(x, y, w, 40, C_BG);
  cv.drawRect(x, y, w, 40, C_WARN);
  cv.setTextColor(C_FG, C_BG);
  cv.setCursor(x + 6, y + 7);
  cv.print(question);
  cv.setTextColor(C_WARN, C_BG);
  cv.setCursor(x + 6, y + 24);
  cv.print("Y = yes     any other key = no");
  push();
  while (true) {
    Key k = poll();
    if (k.any) return k.lc == 'y';
    delay(10);
  }
}

bool input(const char* title, String& value, int maxLen) {
  bool prev = s_repeatAll;
  s_repeatAll = true;
  bool ok = false, dirty = true;
  while (true) {
    Key k = poll();
    if (k.g0 || (k.fn && (k.ch == '`' || k.ch == '~'))) break;
    if (k.enter) { ok = true; break; }
    if (k.del) { if (value.length()) value.remove(value.length() - 1); dirty = true; }
    else if (k.ch >= 32 && k.ch < 127 && !k.fn && (int)value.length() < maxLen) { value += k.ch; dirty = true; }
    if (dirty) {
      dirty = false;
      const int x = 6, y = 36, w = SCR_W - 12, h = 58;
      cv.fillRect(x, y, w, h, C_BG);
      cv.drawRect(x, y, w, h, C_ACC);
      cv.setTextColor(C_ACC, C_BG); cv.setCursor(x + 6, y + 6); cv.print(title);
      cv.drawRect(x + 5, y + 20, w - 10, 14, C_DIM);
      String show = value.length() > 34 ? value.substring(value.length() - 34) : value;
      cv.setTextColor(C_FG, C_BG); cv.setCursor(x + 9, y + 23); cv.print(show);
      cv.fillRect(x + 9 + show.length() * 6, y + 22, 6, 9, C_ACC);
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(x + 6, y + 42); cv.print("ENTER ok    G0 or Fn+` cancel");
      push();
    }
    delay(10);
  }
  s_repeatAll = prev;
  return ok;
}

int popup(const char* title, const char* const* items, int count) {
  int sel = 0;
  bool dirty = true;
  const int rows = min(count, 11), w = 150, h = rows * LINE_H + 18, x = (SCR_W - w) / 2, y = (SCR_H - h) / 2;
  int top = 0;
  while (true) {
    Key k = poll();
    if (k.back || k.tab) return -1;
    if (k.enter) return sel;
    if (k.up) { sel = (sel + count - 1) % count; dirty = true; }
    if (k.down) { sel = (sel + 1) % count; dirty = true; }
    // first letter jumps straight to that entry
    if (k.lc >= 'a' && k.lc <= 'z') for (int i = 0; i < count; i++) if (tolower(items[i][0]) == k.lc) { sel = i; dirty = true; break; }
    if (dirty) {
      dirty = false;
      if (sel < top) top = sel;
      if (sel >= top + rows) top = sel - rows + 1;
      cv.fillRect(x, y, w, h, C_BG);
      cv.drawRect(x, y, w, h, C_ACC);
      cv.fillRect(x + 1, y + 1, w - 2, 11, C_HDR);
      cv.setTextColor(C_ACC, C_HDR); cv.setCursor(x + 5, y + 3); cv.print(title);
      for (int r = 0; r < rows; r++) {
        int yy = y + 15 + r * LINE_H;
        bool on = top + r == sel;
        if (on) cv.fillRect(x + 2, yy - 1, w - 4, LINE_H, C_SEL);
        cv.setTextColor(on ? C_FG : C_DIM, on ? C_SEL : C_BG); cv.setCursor(x + 6, yy); cv.print(items[top + r]);
      }
      push();
    }
    delay(10);
  }
}

// ---------------------------------------------------------------- SD card
// Cardputer / Cardputer ADV microSD: SCK 40, MISO 39, MOSI 14, CS 12
bool sdMount() {
  if (s_sdOk) return true;
  s_sdTried = true;
  SPI.begin(40, 39, 14, 12);
  s_sdOk = SD.begin(12, SPI, 25000000);
  if (s_sdOk && SD.cardType() == CARD_NONE) { SD.end(); s_sdOk = false; }
  if (s_sdOk && !SD.exists("/SensorLab")) SD.mkdir("/SensorLab");
  return s_sdOk;
}

bool sdReady() {
  if (!s_sdTried) sdMount();
  return s_sdOk;
}

String nextFile(const char* prefix, const char* ext) {
  uint32_t seq = prefs.getUInt("seq", 0);
  char path[64];
  do {
    seq++;
    snprintf(path, sizeof(path), "/SensorLab/%s_%04u.%s", prefix, (unsigned)seq, ext);
  } while (SD.exists(path));
  prefs.putUInt("seq", seq);
  return String(path);
}

bool saveLines(const char* prefix, const std::vector<String>& lines) {
  if (!sdMount()) { toast("No SD card", C_BAD); return false; }
  String path = nextFile(prefix, "txt");
  File f = SD.open(path, FILE_WRITE);
  if (!f) { toast("SD write failed", C_BAD); return false; }
  for (auto& l : lines) {
    String s = l;
    s.replace("\t", ": ");
    f.println(s);
  }
  f.close();
  toast("Saved " + path.substring(11));
  return true;
}

// ---------------------------------------------------------------- pager
// Wraps "label<TAB>value" lines whose value would run past the right edge; continuation lines
// keep the value column. Section titles ('#') are left alone.
static void wrapLines(std::vector<String>& in) {
  std::vector<String> out;
  for (auto& l : in) {
    if (l.startsWith("#")) { out.push_back(l); continue; }
    int tab = l.indexOf('\t');
    if (tab < 0) {
      String rest = l;
      while ((int)rest.length() > 39) {
        int cut = rest.lastIndexOf(' ', 39);
        if (cut < 20) cut = 39;
        out.push_back(rest.substring(0, cut));
        rest = rest.substring(rest[cut] == ' ' ? cut + 1 : cut);
      }
      out.push_back(rest);
      continue;
    }
    String label = l.substring(0, tab), val = l.substring(tab + 1);
    int x = 2 + (tab + 1) * 6;
    if (x < 80) x = 80;
    int avail = (SCR_W - x) / 6;
    bool first = true;
    while ((int)val.length() > avail) {
      int cut = val.lastIndexOf(' ', avail);
      if (cut < avail / 2) cut = avail;
      out.push_back((first ? label : String(" ")) + "\t" + val.substring(0, cut));
      val = val.substring(val[cut] == ' ' ? cut + 1 : cut);
      first = false;
      avail = (SCR_W - 80) / 6;
    }
    out.push_back((first ? label : String(" ")) + "\t" + val);
  }
  in.swap(out);
}

void pager(const char* title, std::function<void(std::vector<String>&)> fill, uint32_t refreshMs,
           const char* savePrefix) {
  std::vector<String> lines;
  fill(lines);
  wrapLines(lines);
  int top = 0;
  uint32_t last = millis();
  bool dirty = true;
  while (true) {
    Key k = poll();
    if (k.back) return;
    int maxTop = (int)lines.size() - ROWS;
    if (maxTop < 0) maxTop = 0;
    if (k.up) { top--; dirty = true; }
    if (k.down) { top++; dirty = true; }
    if (k.left) { top -= ROWS; dirty = true; }
    if (k.right) { top += ROWS; dirty = true; }
    if (k.lc == 's' && savePrefix) { saveLines(savePrefix, lines); dirty = true; }
    if (top > maxTop) top = maxTop;
    if (top < 0) top = 0;
    if (refreshMs && millis() - last >= refreshMs) {
      lines.clear();
      fill(lines);
      wrapLines(lines);
      last = millis();
      dirty = true;
    }
    if (dirty) {
      dirty = false;
      clear();
      char pos[16];
      snprintf(pos, sizeof(pos), "%d-%d/%d", lines.empty() ? 0 : top + 1,
               min((int)lines.size(), top + ROWS), (int)lines.size());
      header(title, pos);
      for (int r = 0; r < ROWS && top + r < (int)lines.size(); r++) {
        const String& l = lines[top + r];
        int y = TOP + r * LINE_H;
        if (l.startsWith("#")) {
          cv.fillRect(0, y - 1, SCR_W, LINE_H, C_SEL);
          cv.setTextColor(C_YEL, C_SEL);
          cv.setCursor(2, y);
          cv.print(l.substring(1));
          continue;
        }
        int tab = l.indexOf('\t');
        if (tab < 0) {
          cv.setTextColor(C_FG, C_BG);
          cv.setCursor(2, y);
          cv.print(l);
        } else {
          cv.setTextColor(C_DIM, C_BG);
          cv.setCursor(2, y);
          cv.print(l.substring(0, tab));
          cv.setTextColor(C_FG, C_BG);
          int x = 2 + (tab + 1) * 6;
          if (x < 80) x = 80;
          cv.setCursor(x, y);
          cv.print(l.substring(tab + 1));
        }
      }
      footer(savePrefix ? ";/. scroll  ,// page  S save  ` back" : ";/. scroll  ,// page  ` back");
      push();
    }
    delay(15);
  }
}

String hex(const uint8_t* d, size_t n, char sep) {
  String s;
  char b[4];
  for (size_t i = 0; i < n; i++) {
    snprintf(b, sizeof(b), "%02X", d[i]);
    s += b;
    if (sep && i + 1 < n) s += sep;
  }
  return s;
}

String mac(const uint8_t* m) { return hex(m, 6, ':'); }

}  // namespace ui
