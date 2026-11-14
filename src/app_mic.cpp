// Microphone: live spectrum analyser (bars / spectrogram / oscilloscope) + WAV recorder.
// ADV audio path: ES8311 codec on I2S (BCK 41, WS 43, DIN 46), shared with the speaker.
#include "apps.h"
#include "ui.h"
#include <math.h>

static const int SR = 16000;      // sample rate
static const int N = 1024;        // samples per block (64 ms) and FFT size
static const int NBUF = 4;
static const int NBARS = 56;

static float s_re[N], s_im[N], s_win[N];
static float s_db[N / 2];
static bool s_winReady = false;

static void fft(float* re, float* im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = a + len / 2;
        float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi;
        re[a] += xr; im[a] += xi;
        float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

static void writeWavHeader(File& f, uint32_t dataBytes) {
  uint8_t h[44];
  auto u32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
  auto u16 = [&](int o, uint16_t v) { h[o] = v; h[o + 1] = v >> 8; };
  memcpy(h, "RIFF", 4); u32(4, 36 + dataBytes); memcpy(h + 8, "WAVEfmt ", 8);
  u32(16, 16); u16(20, 1); u16(22, 1); u32(24, SR); u32(28, SR * 2); u16(32, 2); u16(34, 16);
  memcpy(h + 36, "data", 4); u32(40, dataBytes);
  f.seek(0);
  f.write(h, 44);
}

static uint16_t heat(float v) {   // 0..1 -> black/blue/cyan/yellow/red
  if (v < 0) v = 0;
  if (v > 1) v = 1;
  uint8_t r, g, b;
  if (v < 0.25f) { r = 0; g = 0; b = v * 4 * 255; }
  else if (v < 0.5f) { r = 0; g = (v - 0.25f) * 4 * 255; b = 255; }
  else if (v < 0.75f) { r = (v - 0.5f) * 4 * 255; g = 255; b = 255 - r; }
  else { r = 255; g = 255 - (v - 0.75f) * 4 * 255; b = 0; }
  return cv.color565(r, g, b);
}

void appMic() {
  if (!s_winReady) {
    for (int i = 0; i < N; i++) s_win[i] = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1));
    s_winReady = true;
  }
  int16_t* buf = (int16_t*)calloc(NBUF * N, sizeof(int16_t));
  M5Canvas wf(&cv);                       // spectrogram strip
  wf.setColorDepth(16);
  const int WF_H = 96;
  const int WF_X = 16, WF_W = SCR_W - WF_X;          // left gutter holds the frequency labels
  bool wfOk = buf && wf.createSprite(WF_W, WF_H);
  if (!buf) { ui::toast("Out of memory", C_BAD); return; }
  if (wfOk) wf.fillSprite(C_BG);

  // Gain is applied in the ES8311 codec's analogue PGA (0..30 dB in 3 dB steps), which keeps the
  // noise floor down compared with multiplying the samples afterwards.
  int pga = 8;                                        // x3 dB -> +24 dB by default
  auto setPga = [&]() { M5.In_I2C.writeRegister8(0x18, 0x14, 0x10 | pga, 100000); };
  auto micStart = [&]() {
    M5Cardputer.Speaker.end();
    auto mc = M5Cardputer.Mic.config();
    mc.sample_rate = SR;
    mc.magnification = 16;
    mc.noise_filter_level = 0;
    M5Cardputer.Mic.config(mc);
    bool ok = M5Cardputer.Mic.begin();
    delay(20);
    setPga();
    return ok;
  };
  auto micStop = [&]() {
    M5Cardputer.Mic.end();
  };
  if (!micStart()) {
    ui::toast("Microphone init failed", C_BAD, 1500);
    free(buf);
    return;
  }

  int view = 0, idx = 0;
  float peakHold[NBARS] = {0};
  float peakHz = 0, rmsDb = -99, maxDb = -99;
  bool recording = false, hold = false;
  File rec;
  String recName;
  uint32_t recBytes = 0, blocks = 0, clipUntil = 0, lastBlock = millis();

  auto recStop = [&]() {
    if (!recording) return;
    recording = false;
    writeWavHeader(rec, recBytes);
    rec.close();
    ui::toast("Saved " + recName.substring(11) + "  " + String(recBytes / 1024) + " KB");
  };

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.left) view = (view + 2) % 3;
    if (k.right || k.lc == 'v') view = (view + 1) % 3;
    if (k.lc == 'h') hold = !hold;
    if (k.up || k.down) {
      pga = constrain(pga + (k.up ? 1 : -1), 0, 10);
      setPga();
    }
    if (k.enter || k.lc == 'r') {
      if (recording) {
        recStop();
      } else if (!ui::sdMount()) {
        ui::toast("No SD card", C_BAD);
      } else {
        recName = ui::nextFile("rec", "wav");
        rec = SD.open(recName, FILE_WRITE);
        if (!rec) {
          ui::toast("SD write failed", C_BAD);
        } else {
          recBytes = 0;
          writeWavHeader(rec, 0);
          recording = true;
        }
      }
    }

    // Queue the next block; the block queued two calls ago is now complete.
    if (!M5Cardputer.Mic.record(&buf[idx * N], N, SR)) {
      if (millis() - lastBlock > 1500) {          // codec not delivering audio: say so instead of freezing
        ui::clear(); ui::header("MIC");
        cv.setTextColor(C_BAD, C_BG); cv.setCursor(4, 50); cv.print("No audio data from the microphone.");
        ui::footer("` back"); ui::push();
      }
      delay(2);
      continue;
    }
    lastBlock = millis();
    int16_t* d = &buf[((idx + 2) % NBUF) * N];
    idx = (idx + 1) % NBUF;
    blocks++;
    if (blocks < 3) continue;

    if (recording) {
      size_t w = rec.write((const uint8_t*)d, N * 2);
      recBytes += w;
      if (w != (size_t)N * 2) { recStop(); ui::toast("SD full / write error", C_BAD, 1500); }
      else if ((blocks & 15) == 0) rec.flush();
    }
    if (hold) { delay(5); continue; }

    // ---- analysis
    double sum = 0, sq = 0;
    int pk = 0;
    for (int i = 0; i < N; i++) { sum += d[i]; int a = abs(d[i]); if (a > pk) pk = a; }
    float mean = sum / N;
    for (int i = 0; i < N; i++) {
      float v = d[i] - mean;
      sq += v * v;
      s_re[i] = v * s_win[i];
      s_im[i] = 0;
    }
    if (pk > 32000) clipUntil = millis() + 400;
    rmsDb = 20.0f * log10f(sqrtf(sq / N) / 32768.0f + 1e-9f);
    fft(s_re, s_im, N);
    int pkBin = 4;
    maxDb = -120;
    for (int i = 1; i < N / 2; i++) {
      float mag = sqrtf(s_re[i] * s_re[i] + s_im[i] * s_im[i]) / (N * 0.25f);   // Hann-corrected amplitude
      s_db[i] = 20.0f * log10f(mag / 32768.0f + 1e-9f);
      if (i >= 4 && s_db[i] > maxDb) { maxDb = s_db[i]; pkBin = i; }           // peak search ignores < 60 Hz rumble
    }
    peakHz = pkBin * (float)SR / N;
    const float FLOOR = -96, CEIL = -6;
    auto norm = [&](float db) { return (db - FLOOR) / (CEIL - FLOOR); };

    // ---- draw
    ui::clear();
    char right[24];
    if (recording) snprintf(right, sizeof(right), "REC %02u:%02u", (unsigned)(recBytes / (SR * 2) / 60), (unsigned)(recBytes / (SR * 2) % 60));
    else snprintf(right, sizeof(right), "gain +%ddB", pga * 3);
    ui::header(view == 0 ? "MIC Spectrum" : view == 1 ? "MIC Spectrogram" : "MIC Scope", right);
    if (recording) cv.fillCircle(SCR_W - 62, 5, 3, C_BAD);

    cv.setTextColor(C_FG, C_BG);
    cv.setCursor(2, TOP);
    cv.printf("peak %5.0f Hz %4.0f dB", peakHz, maxDb);
    cv.setTextColor(millis() < clipUntil ? C_BAD : C_OK, C_BG);
    cv.setCursor(150, TOP);
    cv.printf("rms %4.0f dBFS", rmsDb);

    const int gy = TOP + 10, gh = BOTTOM - gy - 9;     // graph area
    if (view == 0) {
      // log-spaced bars 50 Hz .. 8 kHz
      const float fLo = 50, fHi = SR / 2;
      int bw = SCR_W / NBARS;
      for (int b = 0; b < NBARS; b++) {
        float f0 = fLo * powf(fHi / fLo, (float)b / NBARS), f1 = fLo * powf(fHi / fLo, (float)(b + 1) / NBARS);
        int i0 = max(1, (int)(f0 * N / SR)), i1 = max(i0 + 1, (int)(f1 * N / SR));
        if (i1 > N / 2) i1 = N / 2;
        float m = -120;
        for (int i = i0; i < i1; i++) if (s_db[i] > m) m = s_db[i];
        float v = norm(m);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        peakHold[b] = max(v, peakHold[b] - 0.012f);
        int h = v * gh, x = b * bw + 8;
        cv.fillRect(x, gy + gh - h, bw - 1, h, heat(0.25f + v * 0.75f));
        cv.drawFastHLine(x, gy + gh - (int)(peakHold[b] * gh), bw - 1, C_FG);
      }
      cv.setTextColor(C_DIM, C_BG);
      static const int marks[] = {100, 500, 1000, 2000, 4000, 8000};
      for (int mhz : marks) {
        float p = logf(mhz / fLo) / logf(fHi / fLo);
        int x = 8 + p * NBARS * bw;
        if (x > SCR_W - 18) x = SCR_W - 18;
        cv.setCursor(x - 6, gy + gh + 1);
        if (mhz >= 1000) cv.printf("%dk", mhz / 1000); else cv.printf("%d", mhz);
      }
    } else if (view == 1 && wfOk) {
      // spectrogram: time scrolls left, frequency up (linear 0..8 kHz)
      wf.scroll(-1, 0);
      for (int y = 0; y < WF_H; y++) {
        int i0 = 1 + y * (N / 2 - 1) / WF_H, i1 = 1 + (y + 1) * (N / 2 - 1) / WF_H;
        float m = -120;
        for (int i = i0; i <= i1 && i < N / 2; i++) if (s_db[i] > m) m = s_db[i];
        wf.drawPixel(WF_W - 1, WF_H - 1 - y, heat(norm(m)));
      }
      wf.pushSprite(&cv, WF_X, gy);
      cv.setTextColor(C_DIM, C_BG);
      cv.setCursor(2, gy); cv.print("8k");
      cv.setCursor(2, gy + WF_H / 4 - 4); cv.print("6k");
      cv.setCursor(2, gy + WF_H / 2 - 4); cv.print("4k");
      cv.setCursor(2, gy + WF_H * 3 / 4 - 4); cv.print("2k");
      cv.setCursor(8, gy + WF_H - 8); cv.print("0");
    } else {
      // oscilloscope: one block, trigger on rising zero crossing
      int start = 0;
      for (int i = 1; i < N - SCR_W * 2; i++) if (d[i - 1] < mean && d[i] >= mean) { start = i; break; }
      int mid = gy + gh / 2, py = mid;
      cv.drawFastHLine(0, mid, SCR_W, C_SEL);
      float sc = pk > 0 ? min(64.0f, 30000.0f / pk) : 1;
      for (int x = 0; x < SCR_W; x++) {
        int y = mid - (int)((d[start + x * 2] - mean) * sc * (gh / 2) / 32768.0f);
        if (y < gy) y = gy;
        if (y > gy + gh) y = gy + gh;
        if (x) cv.drawLine(x - 1, py, x, y, C_OK);
        py = y;
      }
      cv.setTextColor(C_DIM, C_BG);
      cv.setCursor(2, gy + gh + 1);
      cv.printf("30 ms window  zoom x%.0f  peak %d", sc, pk);
    }
    ui::footer(recording ? "ENTER stop  ,// view  ;/. gain  H hold" : "ENTER rec  ,// view  ;/. gain  H hold");
    ui::push();
  }

  recStop();
  micStop();
  wf.deleteSprite();
  free(buf);
  M5Cardputer.Speaker.begin();
}
