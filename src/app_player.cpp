// Sound Player: browse the SD card and play WAV files (PCM 8/16-bit, mono/stereo, any rate),
// including the recordings made by the Mic tool. Audio is streamed from the card in chunks.
#include "apps.h"
#include "ui.h"
#include <algorithm>

struct WavInfo {
  uint16_t channels = 0, bits = 0;
  uint32_t rate = 0, dataStart = 0, dataBytes = 0;
  uint16_t blockAlign = 0;
  const char* error = nullptr;
};

static uint32_t rd32(File& f) { uint8_t b[4] = {0}; f.read(b, 4); return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint16_t rd16(File& f) { uint8_t b[2] = {0}; f.read(b, 2); return b[0] | (b[1] << 8); }

static WavInfo parseWav(File& f) {
  WavInfo w;
  char id[5] = {0};
  f.seek(0);
  f.read((uint8_t*)id, 4);
  if (strncmp(id, "RIFF", 4)) { w.error = "not a RIFF/WAV file"; return w; }
  rd32(f);
  f.read((uint8_t*)id, 4);
  if (strncmp(id, "WAVE", 4)) { w.error = "not a WAVE file"; return w; }
  uint16_t fmt = 0;
  // walk the chunks: "fmt " describes the audio, "data" holds it; anything else is skipped
  while (f.available() >= 8) {
    f.read((uint8_t*)id, 4);
    uint32_t len = rd32(f);
    uint32_t next = f.position() + len + (len & 1);
    if (!strncmp(id, "fmt ", 4)) {
      fmt = rd16(f); w.channels = rd16(f); w.rate = rd32(f); rd32(f); w.blockAlign = rd16(f); w.bits = rd16(f);
    } else if (!strncmp(id, "data", 4)) {
      w.dataStart = f.position();
      w.dataBytes = min(len, (uint32_t)(f.size() - f.position()));
      if (w.dataBytes == 0) w.dataBytes = f.size() - f.position();     // header never finalised
      break;
    }
    if (next >= f.size()) break;
    f.seek(next);
  }
  if (!w.rate || !w.dataStart) w.error = "no audio data found";
  else if (fmt != 1 && fmt != 0xFFFE) w.error = "compressed WAV not supported";
  else if (w.bits != 8 && w.bits != 16) w.error = "only 8/16-bit PCM supported";
  else if (w.channels < 1 || w.channels > 2) w.error = "only mono/stereo supported";
  if (!w.blockAlign) w.blockAlign = w.channels * w.bits / 8;
  return w;
}

struct PEnt { String name; bool dir; uint32_t size; };

static bool isWav(const String& n) {
  String l = n; l.toLowerCase();
  return l.endsWith(".wav");
}

static void listAudio(const String& path, std::vector<PEnt>& out) {
  out.clear();
  File d = SD.open(path);
  if (!d || !d.isDirectory()) return;
  while (out.size() < 200) {
    File f = d.openNextFile();
    if (!f) break;
    String n = f.name();
    int sl = n.lastIndexOf('/');
    if (sl >= 0) n = n.substring(sl + 1);
    bool dir = f.isDirectory();
    if ((dir && !n.startsWith(".") && n != "System Volume Information") || (!dir && isWav(n))) out.push_back({n, dir, (uint32_t)f.size()});
    f.close();
  }
  d.close();
  std::sort(out.begin(), out.end(), [](const PEnt& a, const PEnt& b) { return a.dir != b.dir ? a.dir : a.name < b.name; });
}

static int s_volume = 120;

// Plays one file. Returns +1 when it reached the end, 0 when the user stopped it, -1 on error.
int playerPlay(const String& path, const String& name) {
  File f = SD.open(path);
  if (!f) { ui::toast("Cannot open file", C_BAD); return -1; }
  WavInfo w = parseWav(f);
  if (w.error) { f.close(); ui::toast(w.error, C_BAD, 1500); return -1; }

  const int CH = 0, NB = 3, SAMPLES = 4096;                 // three buffers used in rotation
  int16_t* buf = (int16_t*)malloc(NB * SAMPLES * sizeof(int16_t));
  if (!buf) { f.close(); ui::toast("Out of memory", C_BAD); return -1; }
  auto& spk = M5Cardputer.Speaker;
  spk.begin();
  spk.setVolume(s_volume);
  spk.stop(CH);
  f.seek(w.dataStart);

  const uint32_t bytesPerSec = w.rate * w.blockAlign;
  uint32_t pos = 0;                                         // bytes of audio handed to the speaker
  int bi = 0, result = 0, peak = 0;
  bool paused = false, done = false;
  uint32_t lastDraw = 0;
  int16_t scope[120] = {0};

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.space || k.enter) paused = !paused;
    if (k.up || k.down) { s_volume = constrain(s_volume + (k.up ? 15 : -15), 0, 255); spk.setVolume(s_volume); }
    if (k.left || k.right) {                                // seek 5 s
      int64_t np = (int64_t)pos + (k.right ? 5 : -5) * (int64_t)bytesPerSec;
      if (np < 0) np = 0;
      if (np > w.dataBytes) np = w.dataBytes;
      pos = (uint32_t)np - ((uint32_t)np % w.blockAlign);
      spk.stop(CH);
      f.seek(w.dataStart + pos);
      done = false;
    }
    // keep the speaker's two-slot queue topped up
    if (!paused && !done && spk.isPlaying(CH) < 2) {
      int16_t* b = &buf[bi * SAMPLES];
      uint32_t want = min((uint32_t)(SAMPLES * (w.bits / 8)), w.dataBytes - pos);
      want -= want % w.blockAlign;
      int got = want ? f.read((uint8_t*)b, want) : 0;
      if (got <= 0) {
        done = true;
      } else {
        int n = got / (w.bits / 8);
        if (w.bits == 8) {                                  // unsigned 8-bit -> signed 16-bit, in place from the end
          uint8_t* u = (uint8_t*)b;
          for (int i = n - 1; i >= 0; i--) b[i] = ((int16_t)u[i] - 128) << 8;
        }
        peak = 0;
        for (int i = 0; i < n; i += 8) { int a = abs(b[i]); if (a > peak) peak = a; }
        int step = max(1, n / 120);
        for (int i = 0; i < 120; i++) scope[i] = b[min(n - 1, i * step)];
        spk.playRaw(b, n, w.rate, w.channels == 2, 1, CH, false);
        pos += got;
        bi = (bi + 1) % NB;
      }
    }
    if (done && !spk.isPlaying(CH)) { result = 1; break; }
    if (millis() - lastDraw < 100) { delay(2); continue; }
    lastDraw = millis();

    uint32_t cur = pos / bytesPerSec, tot = w.dataBytes / bytesPerSec;
    ui::clear();
    char right[20];
    snprintf(right, sizeof(right), "vol %d%%", s_volume * 100 / 255);
    ui::header(paused ? "Sound Player  PAUSED" : "Sound Player", right);
    cv.setTextColor(C_FG, C_BG); cv.setTextSize(2); cv.setCursor(4, TOP + 3);
    char nm[20]; snprintf(nm, sizeof(nm), "%.19s", name.c_str());
    cv.print(nm); cv.setTextSize(1);
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 23);
    cv.printf("%lu Hz  %d-bit  %s  %lu KB", (unsigned long)w.rate, w.bits, w.channels == 2 ? "stereo" : "mono", (unsigned long)(f.size() / 1024));
    // waveform of the chunk being played
    const int gy = TOP + 35, gh = 40, mid = gy + gh / 2;
    cv.drawRect(0, gy, SCR_W, gh, C_SEL);
    int py = mid;
    for (int i = 0; i < 120; i++) {
      int y = mid - scope[i] * (gh / 2 - 2) / 32768;
      if (i) cv.drawLine(i * 2 - 1, py, i * 2 + 1, y, paused ? C_DIM : C_OK);
      py = y;
    }
    // level meter + progress
    int lv = peak * (SCR_W - 8) / 32768;
    cv.fillRect(4, gy + gh + 3, lv, 4, peak > 30000 ? C_BAD : peak > 16000 ? C_YEL : C_OK);
    const int by = gy + gh + 11;
    cv.drawRect(4, by, SCR_W - 8, 9, C_FG);
    if (w.dataBytes) cv.fillRect(6, by + 2, (int)((uint64_t)pos * (SCR_W - 12) / w.dataBytes), 5, C_ACC);
    cv.setTextColor(C_FG, C_BG); cv.setCursor(4, by + 12);
    cv.printf("%02lu:%02lu / %02lu:%02lu", (unsigned long)(cur / 60), (unsigned long)(cur % 60), (unsigned long)(tot / 60), (unsigned long)(tot % 60));
    ui::footer("SPACE pause ,// seek ;/. vol ` stop");
    ui::push();
  }
  spk.stop(CH);
  f.close();
  delay(30);                     // let the speaker task release the last buffer before it is freed
  free(buf);
  return result;
}

void appPlayer() {
  if (!ui::sdMount()) {
    ui::clear(); ui::header("Sound Player");
    cv.setTextColor(C_BAD, C_BG); cv.setCursor(4, 40); cv.print("No microSD card detected.");
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, 54); cv.print("Recordings are saved to /SensorLab.");
    ui::footer("` back"); ui::push();
    while (!ui::poll().back) delay(20);
    return;
  }
  String path = "/SensorLab";
  std::vector<PEnt> ents;
  listAudio(path, ents);
  int sel = 0, top = 0;
  bool autoNext = false, dirty = true;
  auto full = [&](const String& n) { return (path == "/" ? "" : path) + "/" + n; };

  while (true) {
    ui::Key k = ui::poll();
    if (k.any) dirty = true;
    if (k.back || k.left) {
      if (path == "/") { if (k.back) return; }
      else { int sl = path.lastIndexOf('/'); path = sl <= 0 ? "/" : path.substring(0, sl); listAudio(path, ents); sel = top = 0; }
    }
    if (k.up && sel > 0) sel--;
    if (k.down && sel + 1 < (int)ents.size()) sel++;
    if (k.lc == 'a') autoNext = !autoNext;
    if (k.lc == 'd' && sel < (int)ents.size() && !ents[sel].dir && ui::confirm("Delete " + ents[sel].name + " ?")) {
      SD.remove(full(ents[sel].name));
      listAudio(path, ents);
      if (sel >= (int)ents.size()) sel = max(0, (int)ents.size() - 1);
    }
    if ((k.enter || k.right) && sel < (int)ents.size()) {
      if (ents[sel].dir) {
        path = full(ents[sel].name); listAudio(path, ents); sel = top = 0;
      } else if (k.enter) {
        // play; with auto-next on, carry on through the following files in this folder
        while (true) {
          int r = playerPlay(full(ents[sel].name), ents[sel].name);
          if (r != 1 || !autoNext) break;
          int nx = sel + 1;
          while (nx < (int)ents.size() && ents[nx].dir) nx++;
          if (nx >= (int)ents.size()) break;
          sel = nx;
        }
      }
      dirty = true;
    }
    if (!dirty) { delay(15); continue; }
    dirty = false;
    ui::clear();
    char right[20];
    snprintf(right, sizeof(right), "%d items", (int)ents.size());
    String title = path.length() > 22 ? "..." + path.substring(path.length() - 19) : path;
    ui::header(title.c_str(), right);
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    for (int r = 0; r < ROWS && top + r < (int)ents.size(); r++) {
      auto& e = ents[top + r];
      int y = TOP + r * LINE_H;
      uint16_t bg = (top + r == sel) ? C_SEL : C_BG;
      if (top + r == sel) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
      char nm[32]; snprintf(nm, sizeof(nm), "%-27.27s", e.name.c_str());
      cv.setTextColor(e.dir ? C_ACC : C_FG, bg); cv.setCursor(2, y); cv.print(nm);
      cv.setTextColor(C_DIM, bg); cv.setCursor(174, y);
      if (e.dir) cv.print("<dir>");
      else if (e.size >= 1048576) cv.printf("%.1f MB", e.size / 1048576.0);
      else cv.printf("%lu KB", (unsigned long)(e.size / 1024));
    }
    if (ents.empty()) {
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(30, 54); cv.print("No WAV files in this folder.");
      cv.setCursor(30, 68); cv.print("Record one in Mic Spectrum (ENTER).");
    }
    ui::footer(autoNext ? "ENTER play , up D del A auto-next:ON" : "ENTER play , up D del A auto-next:off");
    ui::push();
  }
}
