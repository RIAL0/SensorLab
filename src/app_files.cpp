// File Manager for the microSD card: browse, open by type, copy / cut / paste, rename,
// new folder, new text file, delete (recursive), file and folder information, full SD card report.
// Also provides the file chooser used by the Text Editor.
#include "apps.h"
#include "ui.h"
#include <algorithm>
#include <time.h>

enum Kind { K_DIR, K_TEXT, K_AUDIO, K_IMAGE, K_BIN, K_OTHER };
struct FEnt { String name; bool dir; uint32_t size; time_t mtime; };

static String s_clip;             // full path of the item on the clipboard
static bool s_clipCut = false;    // true = move on paste

static String lowerExt(const String& n) {
  int dot = n.lastIndexOf('.');
  if (dot < 0) return "";
  String e = n.substring(dot + 1);
  e.toLowerCase();
  return e;
}

bool filesIsText(const String& name) {
  static const char* const T[] = {"txt", "md", "csv", "log", "json", "conf", "cfg", "ini", "js", "py", "htm", "html",
                                  "xml", "yaml", "yml", "c", "h", "cpp", "sh", "bat", "sub", "ir", "ino", "css"};
  String e = lowerExt(name);
  for (auto t : T) if (e == t) return true;
  return false;
}

static Kind kindOf(const FEnt& e) {
  if (e.dir) return K_DIR;
  String x = lowerExt(e.name);
  if (filesIsText(e.name)) return K_TEXT;
  if (x == "wav" || x == "mp3" || x == "ogg" || x == "flac") return K_AUDIO;
  if (x == "png" || x == "jpg" || x == "jpeg" || x == "bmp" || x == "gif") return K_IMAGE;
  if (x == "bin" || x == "hex" || x == "elf" || x == "uf2") return K_BIN;
  return K_OTHER;
}
static const char* kindName(Kind k) {
  switch (k) {
    case K_DIR: return "folder";
    case K_TEXT: return "text";
    case K_AUDIO: return "audio";
    case K_IMAGE: return "image";
    case K_BIN: return "firmware / binary";
    default: return "file";
  }
}
static uint16_t kindColor(Kind k) {
  switch (k) {
    case K_DIR: return C_YEL;
    case K_TEXT: return C_FG;
    case K_AUDIO: return C_OK;
    case K_IMAGE: return C_ACC;
    case K_BIN: return C_MAG;
    default: return C_DIM;
  }
}

// 10 x 8 glyph: a tabbed folder, or a page with a folded corner coloured by file type
static void drawKindIcon(int x, int y, Kind k) {
  uint16_t c = kindColor(k);
  if (k == K_DIR) {
    cv.fillRect(x, y, 4, 2, c);
    cv.fillRect(x, y + 1, 9, 6, c);
    cv.drawFastHLine(x + 1, y + 3, 7, C_BG);
  } else {
    cv.drawRect(x + 1, y - 1, 7, 9, c);
    cv.fillTriangle(x + 5, y - 1, x + 7, y - 1, x + 7, y + 1, c);
    if (k == K_TEXT) { cv.drawFastHLine(x + 3, y + 3, 3, c); cv.drawFastHLine(x + 3, y + 5, 3, c); }
    else if (k == K_AUDIO) cv.fillRect(x + 3, y + 3, 3, 3, c);
    else if (k == K_BIN) { cv.drawPixel(x + 3, y + 3, c); cv.drawPixel(x + 5, y + 3, c); cv.drawPixel(x + 4, y + 5, c); }
  }
}

static String human(uint64_t v) {
  char b[24];
  if (v >= 1073741824ULL) snprintf(b, sizeof(b), "%.2f GB", v / 1073741824.0);
  else if (v >= 1048576ULL) snprintf(b, sizeof(b), "%.1f MB", v / 1048576.0);
  else if (v >= 1024ULL) snprintf(b, sizeof(b), "%.1f KB", v / 1024.0);
  else snprintf(b, sizeof(b), "%u B", (unsigned)v);
  return b;
}
static String join(const String& dir, const String& name) { return (dir == "/" ? "" : dir) + "/" + name; }
static String parentOf(const String& path) { int sl = path.lastIndexOf('/'); return sl <= 0 ? "/" : path.substring(0, sl); }
static String baseOf(const String& path) { return path.substring(path.lastIndexOf('/') + 1); }

static void listDir(const String& path, std::vector<FEnt>& out) {
  out.clear();
  File d = SD.open(path);
  if (!d || !d.isDirectory()) return;
  while (out.size() < 300) {
    File f = d.openNextFile();
    if (!f) break;
    out.push_back({baseOf(String(f.name())), f.isDirectory(), (uint32_t)f.size(), f.getLastWrite()});
    f.close();
  }
  d.close();
  std::sort(out.begin(), out.end(), [](const FEnt& a, const FEnt& b) {
    if (a.dir != b.dir) return a.dir;
    String x = a.name, y = b.name;
    x.toLowerCase(); y.toLowerCase();
    return x < y;
  });
}

static void progress(const char* what, const String& name, uint64_t done, uint64_t total) {
  const int x = 10, y = 44, w = SCR_W - 20, h = 44;
  cv.fillRect(x, y, w, h, C_BG);
  cv.drawRect(x, y, w, h, C_ACC);
  cv.setTextColor(C_ACC, C_BG); cv.setCursor(x + 6, y + 6); cv.print(what);
  cv.setTextColor(C_FG, C_BG); cv.setCursor(x + 6, y + 17); cv.printf("%.34s", name.c_str());
  cv.drawRect(x + 6, y + 30, w - 12, 8, C_DIM);
  if (total) cv.fillRect(x + 7, y + 31, (int)(done * (w - 14) / total), 6, C_OK);
  ui::push();
}

static bool copyFile(const String& src, const String& dst) {
  File a = SD.open(src);
  if (!a) return false;
  File b = SD.open(dst, FILE_WRITE);
  if (!b) { a.close(); return false; }
  static uint8_t buf[4096];
  uint64_t total = a.size(), done = 0;
  uint32_t last = 0;
  bool ok = true;
  while (true) {
    int n = a.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (b.write(buf, n) != (size_t)n) { ok = false; break; }
    done += n;
    if (millis() - last > 150) { last = millis(); progress("Copying", baseOf(src), done, total); M5Cardputer.update(); }
  }
  a.close(); b.close();
  if (!ok) SD.remove(dst);
  return ok;
}

static bool copyTree(const String& src, const String& dst, int depth = 0) {
  File s = SD.open(src);
  if (!s) return false;
  bool dir = s.isDirectory();
  s.close();
  if (!dir) return copyFile(src, dst);
  if (depth > 8 || !SD.mkdir(dst)) return false;
  std::vector<FEnt> kids;
  listDir(src, kids);
  for (auto& k : kids) if (!copyTree(join(src, k.name), join(dst, k.name), depth + 1)) return false;
  return true;
}

static bool removeTree(const String& path, int depth = 0) {
  File s = SD.open(path);
  if (!s) return false;
  bool dir = s.isDirectory();
  s.close();
  if (!dir) return SD.remove(path);
  if (depth > 8) return false;
  std::vector<FEnt> kids;
  listDir(path, kids);
  for (auto& k : kids) if (!removeTree(join(path, k.name), depth + 1)) return false;
  return SD.rmdir(path);
}

static void treeStats(const String& path, uint64_t& bytes, uint32_t& files, uint32_t& dirs, int depth = 0) {
  std::vector<FEnt> kids;
  listDir(path, kids);
  for (auto& k : kids) {
    if (k.dir) { dirs++; if (depth < 6) treeStats(join(path, k.name), bytes, files, dirs, depth + 1); }
    else { files++; bytes += k.size; }
  }
}

static bool validName(const String& n) {
  if (!n.length() || n == "." || n == "..") return false;
  for (size_t i = 0; i < n.length(); i++) if (strchr("\\/:*?\"<>|", n[i])) return false;
  return true;
}

// "name.ext" -> "name_copy.ext", "name_copy2.ext", ... until the name is free
static String freeName(const String& dir, const String& name) {
  if (!SD.exists(join(dir, name))) return name;
  int dot = name.lastIndexOf('.');
  String stem = dot > 0 ? name.substring(0, dot) : name, ext = dot > 0 ? name.substring(dot) : "";
  for (int i = 1; i < 100; i++) {
    String c = stem + "_copy" + (i > 1 ? String(i) : String("")) + ext;
    if (!SD.exists(join(dir, c))) return c;
  }
  return name;
}

static String fmtTime(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  if (tmv.tm_year + 1900 < 2001) return "not recorded";
  char b[24];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmv);
  return b;
}

static void showInfo(const String& dir, const FEnt& e) {
  String path = join(dir, e.name);
  uint64_t bytes = 0; uint32_t files = 0, dirs = 0;
  if (e.dir) { ui::toast("Counting...", C_DIM, 1); treeStats(path, bytes, files, dirs); }
  ui::pager("Information", [&](std::vector<String>& L) {
    char b[64];
    L.push_back("Name\t" + e.name);
    L.push_back("Location\t" + dir);
    L.push_back(String("Type\t") + kindName(kindOf(e)) + (e.dir || lowerExt(e.name) == "" ? "" : " (." + lowerExt(e.name) + ")"));
    if (e.dir) {
      snprintf(b, sizeof(b), "Contains\t%lu files, %lu folders", (unsigned long)files, (unsigned long)dirs); L.push_back(b);
      L.push_back("Total size\t" + human(bytes));
    } else {
      snprintf(b, sizeof(b), "Size\t%s (%lu bytes)", human(e.size).c_str(), (unsigned long)e.size); L.push_back(b);
    }
    L.push_back("Modified\t" + fmtTime(e.mtime));
    if (!e.dir) {
      File f = SD.open(path);
      if (f) {
        uint8_t h[16] = {0};
        int n = f.read(h, 16);
        f.close();
        L.push_back("First bytes\t" + ui::hex(h, min(n, 8), ' '));
        if (n >= 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4)) L.push_back("Detected\tWAV audio");
        else if (n >= 4 && h[0] == 0xE9) L.push_back("Detected\tESP32 firmware image");
        else if (n >= 4 && !memcmp(h, "\x89PNG", 4)) L.push_back("Detected\tPNG image");
        else if (n >= 3 && h[0] == 0xFF && h[1] == 0xD8) L.push_back("Detected\tJPEG image");
      }
    }
  }, 0, nullptr);
}

static void hexView(const String& path, const String& name) {
  ui::pager(name.c_str(), [&](std::vector<String>& L) {
    File f = SD.open(path);
    if (!f) { L.push_back("cannot open file"); return; }
    uint8_t b[8];
    char line[48];
    for (uint32_t off = 0; off < 2048; off += 8) {
      int n = f.read(b, 8);
      if (n <= 0) break;
      int p = snprintf(line, sizeof(line), "%04lX ", (unsigned long)off);
      for (int i = 0; i < 8; i++) p += snprintf(line + p, sizeof(line) - p, i < n ? "%02X " : "   ", b[i]);
      line[p++] = ' ';
      for (int i = 0; i < n; i++) line[p++] = (b[i] >= 32 && b[i] < 127) ? b[i] : '.';
      line[p] = 0;
      L.push_back(line);
    }
    if (f.size() > 2048) L.push_back("... first 2 KB shown");
    f.close();
  }, 0, nullptr);
}

// A file with no NUL bytes near its start is treated as text even without a known extension.
static bool looksLikeText(const String& path) {
  File f = SD.open(path);
  if (!f) return false;
  uint8_t b[256];
  int n = f.read(b, sizeof(b));
  f.close();
  for (int i = 0; i < n; i++) if (b[i] == 0) return false;
  return n > 0;
}

// ---- SD card identification register (CID), read straight from the card over SPI.
// The SD library does not expose it, so the command is sent here between the library's own
// transfers. Read-only; if the card does not answer, the report simply says so.
static uint8_t crc7(const uint8_t* d, int n) {
  uint8_t c = 0;
  for (int i = 0; i < n; i++) {
    uint8_t b = d[i];
    for (int k = 0; k < 8; k++) { c <<= 1; if ((b ^ c) & 0x80) c ^= 0x09; b <<= 1; }
  }
  return c & 0x7F;
}

static bool sdReadRegister(uint8_t cmd, uint8_t* out16) {
  const int CS = 12;
  bool ok = false;
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(CS, LOW);
  uint32_t t0 = millis();
  while (SPI.transfer(0xFF) != 0xFF && millis() - t0 < 300) {}          // wait until the card is idle
  uint8_t pkt[6] = {(uint8_t)(0x40 | cmd), 0, 0, 0, 0, 0};
  pkt[5] = (crc7(pkt, 5) << 1) | 1;
  for (uint8_t b : pkt) SPI.transfer(b);
  uint8_t r1 = 0xFF;
  for (int i = 0; i < 10 && (r1 & 0x80); i++) r1 = SPI.transfer(0xFF);
  if (r1 == 0x00) {
    uint8_t tok = 0xFF;
    t0 = millis();
    while ((tok = SPI.transfer(0xFF)) == 0xFF && millis() - t0 < 200) {}
    if (tok == 0xFE) {
      for (int i = 0; i < 16; i++) out16[i] = SPI.transfer(0xFF);
      SPI.transfer(0xFF); SPI.transfer(0xFF);                           // data CRC
      ok = true;
    }
  }
  digitalWrite(CS, HIGH);
  SPI.transfer(0xFF);
  SPI.endTransaction();
  return ok;
}

static const char* sdMaker(uint8_t mid) {
  switch (mid) {
    case 0x01: return "Panasonic";
    case 0x02: return "Toshiba / Kioxia";
    case 0x03: return "SanDisk";
    case 0x1B: return "Samsung";
    case 0x27: return "Phison";
    case 0x28: return "Lexar";
    case 0x41: return "Kingston";
    case 0x74: return "Transcend";
    default: return "not in built-in list";
  }
}

extern "C" {
#include "ff.h"
}

static void cardInfo() {
  uint8_t cid[16] = {0}, csd[16] = {0};
  bool haveCid = sdReadRegister(10, cid);          // CMD10 SEND_CID
  bool haveCsd = sdReadRegister(9, csd);           // CMD9  SEND_CSD
  // filesystem details straight from FatFs
  FATFS* fs = nullptr;
  DWORD freeClust = 0;
  const char* drv = "0:";
  if (f_getfree(drv, &freeClust, &fs) != FR_OK) { drv = "1:"; if (f_getfree(drv, &freeClust, &fs) != FR_OK) fs = nullptr; }
  uint64_t rootBytes = 0; uint32_t rootFiles = 0, rootDirs = 0;
  {
    std::vector<FEnt> top;
    listDir("/", top);
    for (auto& e : top) { if (e.dir) rootDirs++; else { rootFiles++; rootBytes += e.size; } }
  }

  ui::pager("SD Card", [&](std::vector<String>& L) {
    uint8_t t = SD.cardType();
    uint64_t tot = SD.totalBytes(), used = SD.usedBytes();
    char b[64];
    L.push_back("#Card");
    L.push_back(String("Type\t") + (t == CARD_MMC ? "MMC" : t == CARD_SD ? "SDSC (up to 2 GB)" : t == CARD_SDHC ? "SDHC / SDXC (block addressed)" : "unknown"));
    L.push_back("Raw capacity\t" + human(SD.cardSize()));
    snprintf(b, sizeof(b), "Sectors\t%lu x %lu bytes", (unsigned long)SD.numSectors(), (unsigned long)SD.sectorSize()); L.push_back(b);
    L.push_back("#Identification (CID)");
    if (haveCid) {
      snprintf(b, sizeof(b), "Maker\t0x%02X %s", cid[0], sdMaker(cid[0])); L.push_back(b);
      char oem[3] = {(char)(cid[1] >= 32 && cid[1] < 127 ? cid[1] : '?'), (char)(cid[2] >= 32 && cid[2] < 127 ? cid[2] : '?'), 0};
      L.push_back(String("OEM ID\t") + oem);
      char pn[6];
      for (int i = 0; i < 5; i++) pn[i] = (cid[3 + i] >= 32 && cid[3 + i] < 127) ? cid[3 + i] : '?';
      pn[5] = 0;
      L.push_back(String("Product name\t") + pn);
      snprintf(b, sizeof(b), "Revision\t%d.%d", cid[8] >> 4, cid[8] & 0x0F); L.push_back(b);
      snprintf(b, sizeof(b), "Serial number\t0x%02X%02X%02X%02X", cid[9], cid[10], cid[11], cid[12]); L.push_back(b);
      int year = 2000 + (((cid[13] & 0x0F) << 4) | (cid[14] >> 4)), month = cid[14] & 0x0F;
      snprintf(b, sizeof(b), "Manufactured\t%04d-%02d", year, month); L.push_back(b);
      L.push_back("Raw CID\t" + ui::hex(cid, 8));
      L.push_back(" \t" + ui::hex(cid + 8, 8));
    } else {
      L.push_back("card did not return its ID register");
    }
    if (haveCsd) {
      L.push_back("#Card-specific data (CSD)");
      snprintf(b, sizeof(b), "CSD version\t%d.0", (csd[0] >> 6) + 1); L.push_back(b);
      // TRAN_SPEED byte: 0x32 = 25 MHz (default speed), 0x5A = 50 MHz (high speed)
      snprintf(b, sizeof(b), "Max bus speed\t%s", csd[3] == 0x5A ? "50 MHz (high speed)" : csd[3] == 0x32 ? "25 MHz (default)" : "other"); L.push_back(b);
      snprintf(b, sizeof(b), "Read block\t%d bytes", 1 << (csd[5] & 0x0F)); L.push_back(b);
      L.push_back(String("Write protect\t") + ((csd[14] & 0x30) ? "yes" : "no"));
      L.push_back("Raw CSD\t" + ui::hex(csd, 8));
      L.push_back(" \t" + ui::hex(csd + 8, 8));
    }
    L.push_back("#Filesystem");
    if (fs) {
      const char* ft = fs->fs_type == FS_FAT12 ? "FAT12" : fs->fs_type == FS_FAT16 ? "FAT16" : fs->fs_type == FS_FAT32 ? "FAT32" : fs->fs_type == FS_EXFAT ? "exFAT" : "unknown";
      L.push_back(String("Format\t") + ft);
      uint32_t clusterBytes = (uint32_t)fs->csize * SD.sectorSize();
      L.push_back("Cluster size\t" + human(clusterBytes));
      snprintf(b, sizeof(b), "Clusters\t%lu total", (unsigned long)(fs->n_fatent - 2)); L.push_back(b);
      snprintf(b, sizeof(b), "Free clusters\t%lu", (unsigned long)freeClust); L.push_back(b);
    } else {
      L.push_back("Format\tFAT (details unavailable)");
    }
    L.push_back("#Space");
    L.push_back("Volume size\t" + human(tot));
    L.push_back("Used\t" + human(used));
    L.push_back("Free\t" + human(tot - used));
    snprintf(b, sizeof(b), "In use\t%d %%", tot ? (int)(used * 100 / tot) : 0); L.push_back(b);
    L.push_back("#Contents of the root folder");
    snprintf(b, sizeof(b), "Folders\t%lu", (unsigned long)rootDirs); L.push_back(b);
    snprintf(b, sizeof(b), "Files\t%lu (%s)", (unsigned long)rootFiles, human(rootBytes).c_str()); L.push_back(b);
    L.push_back("#Connection");
    L.push_back("Interface\tSPI, 25 MHz");
    L.push_back("Pins\tSCK G40, MISO G39, MOSI G14, CS G12");
    L.push_back("SensorLab data\t/SensorLab");
    L.push_back("#Not available over SPI here");
    L.push_back("Speed class, volume label, wear / health data and error counters are not readable with this build.");
  }, 0, "sdcard");
}

// The browser. pick = true turns it into a file chooser that returns the chosen path.
static String browse(bool pick, const char* pickTitle) {
  if (!ui::sdMount()) {
    ui::clear(); ui::header(pick ? pickTitle : "File Manager");
    cv.setTextColor(C_BAD, C_BG); cv.setCursor(4, 40); cv.print("No microSD card detected.");
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, 54); cv.print("Insert a FAT32 card and reopen this tool.");
    ui::footer("` back"); ui::push();
    while (!ui::poll().back) delay(20);
    return "";
  }
  static String path = "/";
  if (!SD.exists(path)) path = "/";
  std::vector<FEnt> ents;
  listDir(path, ents);
  int sel = 0, top = 0;
  bool dirty = true;
  auto reload = [&](const String& focus = String()) {
    listDir(path, ents);
    if (focus.length()) for (int i = 0; i < (int)ents.size(); i++) if (ents[i].name == focus) sel = i;
    if (sel >= (int)ents.size()) sel = max(0, (int)ents.size() - 1);
    dirty = true;
  };
  auto goUp = [&]() {
    if (path == "/") return false;
    String from = baseOf(path);
    path = parentOf(path); sel = top = 0;
    reload(from);
    return true;
  };

  while (true) {
    ui::Key k = ui::poll();
    if (k.any) dirty = true;
    bool has = sel < (int)ents.size();
    if (k.back) { if (!goUp()) return ""; continue; }
    if (k.left) { goUp(); continue; }
    if (k.up && sel > 0) sel--;
    if (k.down && sel + 1 < (int)ents.size()) sel++;

    // ---- which action?
    enum { A_NONE, A_OPEN, A_INFO, A_COPY, A_CUT, A_PASTE, A_RENAME, A_NEWDIR, A_NEWTXT, A_DELETE, A_HEX, A_CARD };
    int act = A_NONE;
    if (k.enter || k.right) act = A_OPEN;
    if (!pick) {
      if (k.lc == 'i') act = A_INFO;
      if (k.lc == 'c') act = A_COPY;
      if (k.lc == 'x') act = A_CUT;
      if (k.lc == 'v') act = A_PASTE;
      if (k.lc == 'r') act = A_RENAME;
      if (k.lc == 'n') act = A_NEWDIR;
      if (k.lc == 't') act = A_NEWTXT;
      if (k.lc == 'd') act = A_DELETE;
      if (k.lc == 'h') act = A_HEX;
      if (k.lc == 's') act = A_CARD;
      if (k.tab || k.lc == 'm') {
        static const char* const items[] = {"Open", "Info", "Copy", "X  Cut (move)", "V  Paste here", "Rename", "New folder",
                                            "Text file (new)", "Delete", "Hex view", "SD card info"};
        static const int map[] = {A_OPEN, A_INFO, A_COPY, A_CUT, A_PASTE, A_RENAME, A_NEWDIR, A_NEWTXT, A_DELETE, A_HEX, A_CARD};
        int c = ui::popup(has ? ents[sel].name.substring(0, 22).c_str() : "This folder", items, 11);
        if (c >= 0) act = map[c];
        dirty = true;
      }
    }

    switch (act) {
      case A_OPEN:
        if (!has) break;
        if (ents[sel].dir) { path = join(path, ents[sel].name); sel = top = 0; reload(); break; }
        if (pick) return join(path, ents[sel].name);
        {
          String full = join(path, ents[sel].name), nm = ents[sel].name;
          Kind kd = kindOf(ents[sel]);
          if (lowerExt(nm) == "wav") playerPlay(full, nm);
          else if (kd == K_TEXT || (kd == K_OTHER && ents[sel].size < 48 * 1024 && looksLikeText(full))) { editorOpen(full); reload(nm); }
          else hexView(full, nm);
        }
        break;
      case A_INFO: if (has) showInfo(path, ents[sel]); break;
      case A_HEX: if (has && !ents[sel].dir) hexView(join(path, ents[sel].name), ents[sel].name); break;
      case A_CARD: cardInfo(); break;
      case A_COPY:
      case A_CUT:
        if (!has) break;
        s_clip = join(path, ents[sel].name);
        s_clipCut = (act == A_CUT);
        ui::toast(String(s_clipCut ? "Cut: " : "Copied: ") + ents[sel].name.substring(0, 26) + "  (V to paste)", C_OK, 900);
        break;
      case A_PASTE: {
        if (!s_clip.length()) { ui::toast("Clipboard is empty", C_WARN); break; }
        if (!SD.exists(s_clip)) { ui::toast("Source no longer exists", C_BAD); s_clip = ""; break; }
        if (path == s_clip || path.startsWith(s_clip + "/")) { ui::toast("Cannot paste a folder into itself", C_BAD, 1400); break; }
        String name = baseOf(s_clip);
        bool ok;
        if (s_clipCut) {
          if (parentOf(s_clip) == path) { ui::toast("Already in this folder", C_WARN); break; }
          name = freeName(path, name);
          ok = SD.rename(s_clip, join(path, name));
          if (!ok) { ok = copyTree(s_clip, join(path, name)) && removeTree(s_clip); }
          if (ok) s_clip = "";
        } else {
          name = freeName(path, name);
          ok = copyTree(s_clip, join(path, name));
        }
        reload(name);
        ui::toast(ok ? (s_clipCut ? "Moved" : "Pasted as " + name.substring(0, 24)) : String("Operation failed (card full?)"), ok ? C_OK : C_BAD, 1000);
        s_clipCut = false;
        break;
      }
      case A_RENAME: {
        if (!has) break;
        String nn = ents[sel].name;
        if (!ui::input("Rename to", nn, 60) || nn == ents[sel].name) break;
        if (!validName(nn)) { ui::toast("Invalid name", C_BAD); break; }
        if (SD.exists(join(path, nn))) { ui::toast("That name already exists", C_BAD); break; }
        bool ok = SD.rename(join(path, ents[sel].name), join(path, nn));
        reload(nn);
        ui::toast(ok ? "Renamed" : "Rename failed", ok ? C_OK : C_BAD);
        break;
      }
      case A_NEWDIR: {
        String nn = "New folder";
        if (!ui::input("New folder name", nn, 60)) break;
        if (!validName(nn) || SD.exists(join(path, nn))) { ui::toast("Invalid or existing name", C_BAD); break; }
        bool ok = SD.mkdir(join(path, nn));
        reload(nn);
        if (!ok) ui::toast("Could not create folder", C_BAD);
        break;
      }
      case A_NEWTXT: {
        String nn = "note.txt";
        if (!ui::input("New text file name", nn, 60)) break;
        if (nn.indexOf('.') < 0) nn += ".txt";
        if (!validName(nn) || SD.exists(join(path, nn))) { ui::toast("Invalid or existing name", C_BAD); break; }
        File f = SD.open(join(path, nn), FILE_WRITE);
        if (!f) { ui::toast("Could not create file", C_BAD); break; }
        f.close();
        editorOpen(join(path, nn));
        reload(nn);
        break;
      }
      case A_DELETE: {
        if (!has) break;
        String q = ents[sel].dir ? "Delete folder + ALL contents?" : "Delete this file?";
        cv.fillRect(10, 30, SCR_W - 20, 12, C_BG);
        cv.setTextColor(C_WARN, C_BG); cv.setCursor(14, 32); cv.printf("%.34s", ents[sel].name.c_str());
        if (!ui::confirm(q)) break;
        String full = join(path, ents[sel].name);
        bool ok = removeTree(full);
        if (s_clip == full) s_clip = "";
        reload();
        ui::toast(ok ? "Deleted" : "Delete failed", ok ? C_OK : C_BAD);
        break;
      }
      default: break;
    }
    if (!dirty) { delay(15); continue; }
    dirty = false;

    ui::clear();
    char right[24];
    if (s_clip.length() && !pick) snprintf(right, sizeof(right), "%s %d", s_clipCut ? "CUT" : "CLIP", (int)ents.size());
    else snprintf(right, sizeof(right), "%d items", (int)ents.size());
    String title = pick ? String(pickTitle) + " " + path : path;
    if (title.length() > 26) title = "..." + title.substring(title.length() - 23);
    ui::header(title.c_str(), right);
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    for (int r = 0; r < ROWS && top + r < (int)ents.size(); r++) {
      auto& e = ents[top + r];
      int y = TOP + r * LINE_H;
      bool on = top + r == sel;
      uint16_t bg = on ? C_SEL : C_BG;
      if (on) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
      Kind kd = kindOf(e);
      drawKindIcon(2, y, kd);
      bool clipped = s_clip.length() && s_clip == join(path, e.name);
      char nm[32]; snprintf(nm, sizeof(nm), "%-26.26s", e.name.c_str());
      cv.setTextColor(clipped && s_clipCut ? C_DIM : e.dir ? C_YEL : C_FG, bg); cv.setCursor(15, y); cv.print(nm);
      cv.setTextColor(C_DIM, bg);
      String sz = e.dir ? String("folder") : human(e.size);
      cv.setCursor(SCR_W - 2 - sz.length() * 6, y); cv.print(sz);
    }
    if (ents.empty()) { cv.setTextColor(C_DIM, C_BG); cv.setCursor(70, 60); cv.print("(empty folder)"); }
    ui::footer(pick ? "ENTER choose   , up   ` cancel" : "ENTER open TAB menu C X V I  S card");
    ui::push();
  }
}

void appSd() { browse(false, nullptr); }
String filesPick(const char* title) { return browse(true, title); }
