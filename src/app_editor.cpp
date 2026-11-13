// Text Editor for txt / md and other plain-text files on the SD card.
// Because ; . , / are ordinary characters here, cursor movement is on Fn + those keys.
#include "apps.h"
#include "ui.h"

static const uint32_t MAX_BYTES = 48 * 1024;      // whole file is held in RAM
static const int COLS = 39;

struct Doc {
  std::vector<String> lines;
  String path;
  bool dirty = false, readOnly = false;
  int cx = 0, cy = 0, top = 0, hoff = 0;
};

static bool loadDoc(Doc& d) {
  d.lines.clear();
  File f = SD.open(d.path);
  if (!f) return false;
  if (f.size() > MAX_BYTES) d.readOnly = true;         // too big to edit safely: view the first part only
  String cur;
  uint32_t n = 0;
  while (f.available() && n < MAX_BYTES) {
    char c = f.read();
    n++;
    if (c == '\n') { d.lines.push_back(cur); cur = ""; }
    else if (c != '\r') cur += (c == '\t' ? ' ' : c);
  }
  f.close();
  d.lines.push_back(cur);
  return true;
}

static bool saveDoc(Doc& d) {
  String tmp = d.path + ".tmp";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  bool ok = true;
  for (size_t i = 0; i < d.lines.size() && ok; i++) {
    if (d.lines[i].length()) ok = f.print(d.lines[i]) == d.lines[i].length();
    if (ok && i + 1 < d.lines.size()) ok = f.print("\n") == 1;
  }
  f.close();
  if (!ok) { SD.remove(tmp); return false; }
  SD.remove(d.path);                                   // written to a temp file first, so a failed save keeps the original
  if (!SD.rename(tmp, d.path)) return false;
  d.dirty = false;
  return true;
}

static void drawDoc(Doc& d) {
  if (d.cy < d.top) d.top = d.cy;
  if (d.cy >= d.top + ROWS) d.top = d.cy - ROWS + 1;
  if (d.cx < d.hoff) d.hoff = d.cx;
  if (d.cx >= d.hoff + COLS) d.hoff = d.cx - COLS + 1;
  ui::clear();
  String name = d.path.substring(d.path.lastIndexOf('/') + 1);
  if (name.length() > 20) name = name.substring(0, 19) + "~";
  char right[24];
  snprintf(right, sizeof(right), "%d:%d/%d", d.cy + 1, d.cx + 1, (int)d.lines.size());
  ui::header((String(d.readOnly ? "[RO] " : d.dirty ? "* " : "") + name).c_str(), right);
  for (int r = 0; r < ROWS && d.top + r < (int)d.lines.size(); r++) {
    const String& l = d.lines[d.top + r];
    int y = TOP + r * LINE_H;
    // light Markdown colouring: headings, list bullets, quotes
    uint16_t col = C_FG;
    if (l.startsWith("#")) col = C_YEL;
    else if (l.startsWith("- ") || l.startsWith("* ") || l.startsWith("+ ")) col = C_ACC;
    else if (l.startsWith(">")) col = C_DIM;
    else if (l.startsWith("```")) col = C_MAG;
    cv.setTextColor(col, C_BG);
    if ((int)l.length() > d.hoff) { cv.setCursor(1, y); cv.print(l.substring(d.hoff, d.hoff + COLS)); }
    if (d.hoff > 0) cv.drawFastVLine(0, y, 8, C_WARN);
    if ((int)l.length() > d.hoff + COLS) cv.drawFastVLine(SCR_W - 1, y, 8, C_WARN);
    if (d.top + r == d.cy) {
      int x = 1 + (d.cx - d.hoff) * 6;
      cv.fillRect(x, y - 1, 6, 9, C_ACC);
      if (d.cx < (int)l.length()) { cv.setTextColor(C_BG, C_ACC); cv.setCursor(x, y); cv.print(l[d.cx]); }
    }
  }
  ui::footer(d.readOnly ? "Fn+;.,/ move  Ctrl+Q quit  read only" : "Fn+;.,/ move  Ctrl+S save  Ctrl+Q quit");
  ui::push();
}

// Returns true when the editor should close.
static bool askQuit(Doc& d) {
  if (!d.dirty) return true;
  const int x = 10, y = 40, w = SCR_W - 20;
  cv.fillRect(x, y, w, 46, C_BG);
  cv.drawRect(x, y, w, 46, C_WARN);
  cv.setTextColor(C_FG, C_BG); cv.setCursor(x + 6, y + 7); cv.print("Unsaved changes.");
  cv.setTextColor(C_WARN, C_BG);
  cv.setCursor(x + 6, y + 20); cv.print("S = save and quit   D = discard");
  cv.setCursor(x + 6, y + 31); cv.print("any other key = keep editing");
  ui::push();
  while (true) {
    ui::Key k = ui::poll();
    if (!k.any) { delay(10); continue; }
    if (k.lc == 's') {
      if (saveDoc(d)) return true;
      ui::toast("Save failed", C_BAD);
      return false;
    }
    return k.lc == 'd';
  }
}

static void editLoop(Doc& d) {
  ui::setRepeatAll(true);
  bool redraw = true;
  while (true) {
    ui::Key k = ui::poll();
    if (k.any) {
      redraw = true;
      String& line = d.lines[d.cy];
      bool esc = k.g0 || (k.fn && (k.ch == '`' || k.ch == '~')) || (k.ctrl && k.lc == 'q');
      if (esc) {
        if (askQuit(d)) break;
      } else if (k.ctrl && k.lc == 's') {
        if (d.readOnly) ui::toast("Read only (file over 48 KB)", C_WARN);
        else { bool ok = saveDoc(d); ui::toast(ok ? "Saved" : "Save failed", ok ? C_OK : C_BAD, 600); }
      } else if (k.ctrl && k.up) {                        // page up
        d.cy = max(0, d.cy - ROWS);
      } else if (k.ctrl && k.down) {                      // page down
        d.cy = min((int)d.lines.size() - 1, d.cy + ROWS);
      } else if (k.ctrl && k.left) {                      // home
        d.cx = 0;
      } else if (k.ctrl && k.right) {                     // end
        d.cx = line.length();
      } else if (k.fn && k.up) {
        if (d.cy > 0) d.cy--;
      } else if (k.fn && k.down) {
        if (d.cy + 1 < (int)d.lines.size()) d.cy++;
      } else if (k.fn && k.left) {
        if (d.cx > 0) d.cx--;
        else if (d.cy > 0) { d.cy--; d.cx = d.lines[d.cy].length(); }
      } else if (k.fn && k.right) {
        if (d.cx < (int)line.length()) d.cx++;
        else if (d.cy + 1 < (int)d.lines.size()) { d.cy++; d.cx = 0; }
      } else if (d.readOnly || k.fn) {
        // no editing in read-only mode; other Fn combinations are ignored
      } else if (k.ctrl && k.lc == 'k') {                 // delete the whole line
        if (d.lines.size() > 1) d.lines.erase(d.lines.begin() + d.cy); else d.lines[0] = "";
        if (d.cy >= (int)d.lines.size()) d.cy = d.lines.size() - 1;
        d.cx = 0; d.dirty = true;
      } else if (k.ctrl) {
        // unassigned Ctrl combination
      } else if (k.enter) {
        String rest = line.substring(d.cx);
        line = line.substring(0, d.cx);
        d.lines.insert(d.lines.begin() + d.cy + 1, rest);
        d.cy++; d.cx = 0; d.dirty = true;
      } else if (k.del) {
        if (d.cx > 0) { line.remove(d.cx - 1, 1); d.cx--; d.dirty = true; }
        else if (d.cy > 0) {
          d.cx = d.lines[d.cy - 1].length();
          d.lines[d.cy - 1] += line;
          d.lines.erase(d.lines.begin() + d.cy);
          d.cy--; d.dirty = true;
        }
      } else if (k.tab) {
        line = line.substring(0, d.cx) + "  " + line.substring(d.cx);
        d.cx += 2; d.dirty = true;
      } else if (k.ch >= 32 && k.ch < 127) {
        line = line.substring(0, d.cx) + String(k.ch) + line.substring(d.cx);
        d.cx++; d.dirty = true;
      }
      if (d.cx > (int)d.lines[d.cy].length()) d.cx = d.lines[d.cy].length();
    }
    if (redraw) { redraw = false; drawDoc(d); }
    delay(8);
  }
  ui::setRepeatAll(false);
}

void editorOpen(const String& path) {
  Doc d;
  d.path = path;
  if (!loadDoc(d)) { ui::toast("Cannot open file", C_BAD); return; }
  if (d.readOnly) ui::toast("Over 48 KB: first part, read only", C_WARN, 1400);
  editLoop(d);
}

// Stand-alone entry from the main menu: start a new note or choose an existing file.
void appEditor() {
  if (!ui::sdMount()) {
    ui::clear(); ui::header("Text Editor");
    cv.setTextColor(C_BAD, C_BG); cv.setCursor(4, 40); cv.print("No microSD card detected.");
    ui::footer("` back"); ui::push();
    while (!ui::poll().back) delay(20);
    return;
  }
  static const char* const opts[] = {"New note in /SensorLab", "Open a file..."};
  int sel = 0;
  bool dirty = true;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) return;
    if (k.up || k.down) { sel ^= 1; dirty = true; }
    if (k.enter) {
      if (sel == 0) {
        String name = ui::nextFile("note", "txt").substring(11);
        if (ui::input("New note file name", name, 60)) {
          if (name.indexOf('.') < 0) name += ".txt";
          String p = "/SensorLab/" + name;
          if (SD.exists(p)) ui::toast("That name already exists", C_BAD);
          else {
            File f = SD.open(p, FILE_WRITE);
            if (f) { f.close(); editorOpen(p); } else ui::toast("Could not create file", C_BAD);
          }
        }
      } else {
        String p = filesPick("Open:");
        if (p.length()) editorOpen(p);
      }
      dirty = true;
    }
    if (!dirty) { delay(15); continue; }
    dirty = false;
    ui::clear();
    ui::header("Text Editor");
    for (int i = 0; i < 2; i++) {
      int y = TOP + 8 + i * 20;
      if (i == sel) cv.fillRoundRect(6, y - 4, SCR_W - 12, 17, 3, C_SEL);
      cv.setTextColor(i == sel ? C_FG : C_DIM, i == sel ? C_SEL : C_BG); cv.setCursor(14, y); cv.print(opts[i]);
    }
    int y = TOP + 56;
    cv.setTextColor(C_DIM, C_BG);
    cv.setCursor(6, y); cv.print("In the editor:");
    cv.setCursor(6, y + 10); cv.print("Fn + ; . , /   move the cursor");
    cv.setCursor(6, y + 20); cv.print("Ctrl + ; . , / page up/down, home, end");
    cv.setCursor(6, y + 30); cv.print("Ctrl+S save   Ctrl+K delete line");
    cv.setCursor(6, y + 40); cv.print("Ctrl+Q or G0 button   quit");
    ui::footer(";/. select   ENTER open   ` back");
    ui::push();
  }
}
