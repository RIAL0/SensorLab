// Signal Hunt ("fox hunt"): pick one WiFi access point or one BLE device, lock on to it, then walk
// around following its signal strength - big meter, trend, history graph and Geiger-style beeps.
// Receive-only: WiFi is tracked from the target's own beacons, BLE from its advertisements.
#include "apps.h"
#include "settings.h"
#include "ui.h"
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_wifi.h>

void wifiQuietSta();                                                       // app_wifi.cpp
String bleNickLabel(const uint8_t* addrLE);                                // app_ble.cpp
bool bleNickIsMine(const uint8_t* addrLE);
void bleNicknamesLoad();
String bleDescribe(const char* name, const uint8_t* mfg, uint8_t mfgLen, const char* svcList,
                   const char* svcDataUuid, const uint8_t* svcData, uint8_t svcDataLen, int appearance);  // app_ble.cpp

struct Target {
  char name[33];
  uint8_t addr[6];
  uint8_t addrType;      // BLE only
  uint8_t ch;            // WiFi only
  int8_t rssi;
  uint8_t flags;         // BLE: 1 = Apple, 2 = has a name or label, 4 = tagged by the user
  uint32_t seen;
};

// ---- live reading of the locked target, written from the radio callbacks
static volatile int s_rssi = -100;
static volatile uint32_t s_hits = 0, s_lastHit = 0;
static uint8_t s_tgt[6];
static uint8_t s_tgtType = 0;
static volatile bool s_locked = false;

static void wifiSniff(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!s_locked || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) return;
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  if (p->rx_ctrl.sig_len < 24) return;
  if (memcmp(p->payload + 10, s_tgt, 6)) return;        // transmitter address must be the target
  s_rssi = p->rx_ctrl.rssi;
  s_lastHit = millis();
  s_hits = s_hits + 1;
}

// ---- BLE candidates + tracking
static const int MAXT = 48;
static Target* s_list = nullptr;
static int s_n = 0;
static SemaphoreHandle_t s_mux = nullptr;

class HuntCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* a) override {
    const uint8_t* ad = a->getAddress().getNative();
    uint8_t at = a->getAddress().getType();
    int rssi = a->getRSSI();
    if (s_locked) {
      if (!memcmp(ad, s_tgt, 6) && at == s_tgtType) { s_rssi = rssi; s_lastHit = millis(); s_hits = s_hits + 1; }
      return;
    }
    String label;
    std::string mfg = a->haveManufacturerData() ? a->getManufacturerData() : std::string();
    String svcs;
    for (int i = 0; i < a->getServiceUUIDCount() && svcs.length() < 60; i++) { svcs += a->getServiceUUID(i).toString().c_str(); svcs += ' '; }
    std::string sd = a->haveServiceData() ? a->getServiceData(0) : std::string();
    std::string sdu = a->haveServiceData() ? a->getServiceDataUUID(0).toString() : std::string();
    String nkl = bleNickLabel(ad);
    bool nk = nkl.length() > 0;
    if (nk) label = nkl;
    else label = bleDescribe(a->haveName() ? a->getName().c_str() : "", (const uint8_t*)mfg.data(), mfg.size(), svcs.c_str(),
                             sdu.c_str(), (const uint8_t*)sd.data(), sd.size(), a->haveAppearance() ? a->getAppearance() : -1);
    uint8_t fl = (mfg.size() >= 2 && (uint8_t)mfg[0] == 0x4C && mfg[1] == 0x00 ? 1 : 0) | (nk || a->haveName() ? 2 : 0) | (bleNickIsMine(ad) ? 4 : 0);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_list) {
      Target* t = nullptr;
      for (int i = 0; i < s_n; i++) if (!memcmp(s_list[i].addr, ad, 6) && s_list[i].addrType == at) { t = &s_list[i]; break; }
      if (!t && s_n < MAXT) { t = &s_list[s_n++]; memset(t, 0, sizeof(Target)); memcpy(t->addr, ad, 6); t->addrType = at; }
      if (t) {
        t->rssi = rssi; t->seen = millis(); t->flags |= fl;
        if (nk) strlcpy(t->name, label.c_str(), sizeof(t->name));
        else if (label.length() && (!t->name[0] || a->haveName())) strlcpy(t->name, label.c_str(), sizeof(t->name));
      }
    }
    xSemaphoreGive(s_mux);
  }
};
static HuntCb s_cb;

static uint16_t sigColor(int r) { return r > -55 ? C_OK : r > -70 ? C_YEL : r > -85 ? C_WARN : C_BAD; }

// ---- the tracking screen. Returns when the user goes back to the list.
static void track(const Target& t, bool ble, bool& sound, int& volume) {
  memcpy(s_tgt, t.addr, 6);
  s_tgtType = t.addrType;
  s_rssi = t.rssi; s_hits = 0; s_lastHit = millis();
  if (!ble) {
    WiFi.scanDelete();
    wifi_promiscuous_filter_t flt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA};
    esp_wifi_set_promiscuous_filter(&flt);
    esp_wifi_set_promiscuous_rx_cb(&wifiSniff);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(t.ch, WIFI_SECOND_CHAN_NONE);
  }
  s_locked = true;
  auto& spk = M5Cardputer.Speaker;
  spk.begin();
  spk.setVolume(volume);

  const int HIST = 232;
  static int8_t hist[HIST];
  memset(hist, -100, sizeof(hist));
  float ema = t.rssi, slow = t.rssi;
  int peak = t.rssi, lastSeenHits = 0;
  uint32_t lastDraw = 0, lastHist = 0, nextBeep = 0, rateAt = millis(), rate = 0, rateHits = 0;

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.lc == 'm' || k.space) sound = !sound;
    if (k.up || k.down) { volume = constrain(volume + (k.up ? 20 : -20), 20, 255); spk.setVolume(volume); }
    if (k.ch == '0') peak = (int)ema;

    ui::stayAwake();                                  // the hunt screen is watched, not typed on
    uint32_t now = millis();
    int hits = s_hits;
    if (hits != lastSeenHits) {                       // a new reading arrived
      lastSeenHits = hits;
      int r = s_rssi;
      ema = ema * 0.75f + r * 0.25f;                  // fast average: the needle
      slow = slow * 0.97f + r * 0.03f;                // slow average: the reference for warmer / colder
      if (ema > peak) peak = (int)ema;
    }
    bool lost = now - s_lastHit > 4000;
    if (now - rateAt >= 1000) { rate = hits - rateHits; rateHits = hits; rateAt = now; }
    if (now - lastHist >= 250) {
      lastHist = now;
      memmove(hist, hist + 1, HIST - 1);
      hist[HIST - 1] = lost ? -100 : (int8_t)ema;
    }
    // Geiger-style clicks: faster and higher-pitched as the signal gets stronger
    float s01 = constrain((ema + 95.0f) / 60.0f, 0.0f, 1.0f);          // -95 dBm -> 0, -35 dBm -> 1
    if (sound && !lost && now >= nextBeep) {
      spk.tone(500.0f + 1900.0f * s01, s01 > 0.95f ? 120 : 28);
      nextBeep = now + (uint32_t)(900.0f - 840.0f * s01);
    }
    if (now - lastDraw < 80) { delay(3); continue; }
    lastDraw = now;

    ui::clear();
    char right[20];
    if (ble) snprintf(right, sizeof(right), "BLE %lu/s", (unsigned long)rate);
    else snprintf(right, sizeof(right), "ch%d %lu/s", t.ch, (unsigned long)rate);
    char title[32]; snprintf(title, sizeof(title), "Hunt %.20s", t.name[0] ? t.name : ui::mac(t.addr).c_str());
    ui::header(title, right);

    uint16_t col = lost ? C_DIM : sigColor((int)ema);
    // big reading
    cv.setTextColor(col, C_BG); cv.setTextSize(3); cv.setCursor(4, TOP + 3);
    if (lost) cv.print("----"); else cv.printf("%d", (int)roundf(ema));
    cv.setTextSize(1); cv.setCursor(82, TOP + 17); cv.print("dBm");
    // warmer / colder
    float diff = ema - slow;
    int ax = 124, ay = TOP + 4;
    if (lost) { cv.setTextColor(C_BAD, C_BG); cv.setCursor(ax, ay + 2); cv.print("SIGNAL LOST"); cv.setTextColor(C_DIM, C_BG); cv.setCursor(ax, ay + 13); cv.print("move back / wait"); }
    else if (s01 > 0.95f) { cv.setTextColor(C_OK, C_BG); cv.setTextSize(2); cv.setCursor(ax, ay + 2); cv.print("HERE!"); cv.setTextSize(1); }
    else if (diff > 2.0f) { cv.fillTriangle(ax + 8, ay, ax, ay + 14, ax + 16, ay + 14, C_OK); cv.setTextColor(C_OK, C_BG); cv.setTextSize(2); cv.setCursor(ax + 22, ay); cv.print("WARMER"); cv.setTextSize(1); }
    else if (diff < -2.0f) { cv.fillTriangle(ax + 8, ay + 14, ax, ay, ax + 16, ay, C_BAD); cv.setTextColor(C_BAD, C_BG); cv.setTextSize(2); cv.setCursor(ax + 22, ay); cv.print("COLDER"); cv.setTextSize(1); }
    else { cv.fillRect(ax, ay + 5, 16, 4, C_DIM); cv.setTextColor(C_DIM, C_BG); cv.setTextSize(2); cv.setCursor(ax + 22, ay); cv.print("steady"); cv.setTextSize(1); }
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(124, TOP + 21);
    if (!lost) cv.printf("peak %d  ~%.0fm", peak, powf(10.0f, (ble ? -59.0f - ema : -40.0f - ema) / 20.0f));

    // strength bar -100 .. -30 dBm with peak marker
    const int bx = 4, by = TOP + 32, bw = SCR_W - 8, bh = 12;
    cv.drawRect(bx, by, bw, bh, C_FG);
    auto xOf = [&](float r) { return bx + 1 + (int)(constrain((r + 100.0f) / 70.0f, 0.0f, 1.0f) * (bw - 2)); };
    if (!lost) cv.fillRect(bx + 1, by + 1, xOf(ema) - bx - 1, bh - 2, col);
    cv.drawFastVLine(xOf(peak), by - 2, bh + 4, C_FG);
    // history, newest on the right
    const int gy = TOP + 49, gh = 52;
    cv.drawRect(bx, gy, bw, gh, C_SEL);
    for (int i = 0; i < HIST; i++) {
      if (hist[i] <= -100) continue;
      int h = (hist[i] + 100) * (gh - 3) / 70;
      if (h < 1) h = 1;
      if (h > gh - 2) h = gh - 2;
      cv.drawFastVLine(bx + 1 + i, gy + gh - 1 - h, h, sigColor(hist[i]));
    }
    for (int d = -80; d <= -40; d += 20) {            // scale drawn last so the bars cannot hide it
      int y = gy + gh - 2 - (d + 100) * (gh - 3) / 70;
      for (int x = bx + 2; x < bx + bw - 2; x += 4) cv.drawPixel(x, y, C_BG);
      cv.setTextColor(C_FG, C_BG); cv.setCursor(bx + 2, y - 4); cv.print(d);
    }
    ui::footer(sound ? "M mute  ;/. vol  0 reset peak  ` list" : "MUTED  M sound on  0 reset  ` list");
    ui::push();
  }
  s_locked = false;
  spk.stop();
  if (!ble) { esp_wifi_set_promiscuous(false); esp_wifi_set_promiscuous_rx_cb(nullptr); }
}

void appHunt() {
  if (!s_mux) s_mux = xSemaphoreCreateMutex();
  s_list = (Target*)calloc(MAXT, sizeof(Target));
  if (!s_list) { ui::toast("Out of memory", C_BAD); return; }
  bool ble = false, sound = true, radioUp = false;
  int filter = 0;                       // BLE list: 0 all, 1 my tags, 2 near, 3 named, 4 no Apple
  static const char* const FILT[] = {"all", "my tags", "near", "named", "no Apple"};
  bleNicknamesLoad();
  int volume = max(40, (int)cfg.volume / 2), sel = 0, top = 0, ch = 1;
  bool scanning = false;
  uint32_t lastDraw = 0, retryAt = 0;
  NimBLEScan* scan = nullptr;
  std::vector<Target> view;
  s_locked = false;

  auto radioStop = [&]() {
    if (!radioUp) return;
    if (ble) { if (scan) scan->stop(); NimBLEDevice::deinit(true); scan = nullptr; }
    else { WiFi.scanDelete(); WiFi.mode(WIFI_OFF); }
    radioUp = false;
  };
  auto radioStart = [&]() {
    xSemaphoreTake(s_mux, portMAX_DELAY); s_n = 0; xSemaphoreGive(s_mux);
    sel = top = 0; ch = 1; scanning = false; retryAt = millis();
    ui::clear(); ui::header("Signal Hunt"); cv.setTextColor(C_DIM, C_BG); cv.setCursor(50, 60);
    cv.print(ble ? "Starting Bluetooth..." : "Starting WiFi..."); ui::push();
    if (ble) {
      NimBLEDevice::init("");
      scan = NimBLEDevice::getScan();
      scan->setAdvertisedDeviceCallbacks(&s_cb, true);
      scan->setActiveScan(true);
      scan->setInterval(80); scan->setWindow(80);
      scan->setMaxResults(0);
      scan->start(0, nullptr, false);
    } else {
      wifiQuietSta();
    }
    radioUp = true;
  };
  radioStart();

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.tab || k.left || k.right) { radioStop(); ble = !ble; radioStart(); continue; }
    if (k.up && sel > 0) sel--;
    if (k.down && sel + 1 < (int)view.size()) sel++;
    if (k.lc == 'x') { xSemaphoreTake(s_mux, portMAX_DELAY); s_n = 0; xSemaphoreGive(s_mux); sel = 0; }
    if (k.lc == 'f' && ble) { filter = (filter + 1) % 5; sel = top = 0; }
    if (k.lc == 'm') sound = !sound;
    if (k.enter && sel < (int)view.size()) {
      Target t = view[sel];
      track(t, ble, sound, volume);
      scanning = false; retryAt = millis();           // WiFi: resume the channel sweep
    }

    // WiFi candidates: sweep one channel at a time and merge by BSSID
    if (!ble) {
      if (scanning) {
        int n = WiFi.scanComplete();
        if (n >= 0) {
          xSemaphoreTake(s_mux, portMAX_DELAY);
          for (int i = 0; i < n; i++) {
            wifi_ap_record_t* r = (wifi_ap_record_t*)WiFi.getScanInfoByIndex(i);
            if (!r) continue;
            Target* t = nullptr;
            for (int j = 0; j < s_n; j++) if (!memcmp(s_list[j].addr, r->bssid, 6)) { t = &s_list[j]; break; }
            if (!t && s_n < MAXT) { t = &s_list[s_n++]; memset(t, 0, sizeof(Target)); memcpy(t->addr, r->bssid, 6); }
            if (t) { strlcpy(t->name, (const char*)r->ssid, sizeof(t->name)); t->ch = r->primary; t->rssi = r->rssi; t->seen = millis(); }
          }
          xSemaphoreGive(s_mux);
          WiFi.scanDelete();
          scanning = false; ch = ch % 13 + 1; retryAt = millis() + 20;
        } else if (n == WIFI_SCAN_FAILED) { scanning = false; ch = ch % 13 + 1; retryAt = millis() + 150; }
      } else if (millis() >= retryAt) {
        WiFi.scanDelete();
        scanning = WiFi.scanNetworks(true, true, false, 200, ch) == WIFI_SCAN_RUNNING;
        if (!scanning) retryAt = millis() + 150;
      }
    }
    if (millis() - lastDraw < 120) { delay(5); continue; }
    lastDraw = millis();

    String keep = sel < (int)view.size() ? ui::mac(view[sel].addr) : String();
    xSemaphoreTake(s_mux, portMAX_DELAY);
    view.assign(s_list, s_list + s_n);
    xSemaphoreGive(s_mux);
    if (ble && filter)
      view.erase(std::remove_if(view.begin(), view.end(), [&](const Target& t) {
        return filter == 1 ? !(t.flags & 4) : filter == 2 ? t.rssi < -60 : filter == 3 ? !(t.flags & 2) : ((t.flags & 1) && !(t.flags & 4));
      }), view.end());
    std::stable_sort(view.begin(), view.end(), [](const Target& a, const Target& b) { return a.rssi > b.rssi; });
    for (int i = 0; i < (int)view.size(); i++) if (ui::mac(view[i].addr) == keep) sel = i;
    if (sel >= (int)view.size()) sel = max(0, (int)view.size() - 1);

    ui::clear();
    char right[24];
    snprintf(right, sizeof(right), "%d found%s", (int)view.size(), sound ? "" : " MUTE");
    char title[36];
    if (ble && filter) snprintf(title, sizeof(title), "Hunt BLE: %s", FILT[filter]);
    else snprintf(title, sizeof(title), ble ? "Hunt: pick BLE device" : "Hunt: pick WiFi network");
    ui::header(title, right);
    const int rows = ROWS;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    for (int r = 0; r < rows && top + r < (int)view.size(); r++) {
      Target& t = view[top + r];
      int y = TOP + r * LINE_H;
      bool on = top + r == sel, stale = millis() - t.seen > 12000;
      uint16_t bg = on ? C_SEL : C_BG;
      if (on) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
      char nm[28];
      if (t.name[0]) snprintf(nm, sizeof(nm), "%-23.23s", t.name);
      else if (ble) { char a[20]; snprintf(a, sizeof(a), "%02X:%02X:%02X:%02X:%02X:%02X", t.addr[5], t.addr[4], t.addr[3], t.addr[2], t.addr[1], t.addr[0]); snprintf(nm, sizeof(nm), "%s", a); }
      else snprintf(nm, sizeof(nm), "<hidden> %02X:%02X:%02X", t.addr[3], t.addr[4], t.addr[5]);
      cv.setTextColor(stale ? C_DIM : t.name[0] ? C_FG : C_DIM, bg); cv.setCursor(2, y); cv.print(nm);
      if (!ble) { cv.setTextColor(C_ACC, bg); cv.setCursor(150, y); cv.printf("%2d", t.ch); }
      else if (t.addrType != 0) { cv.setTextColor(C_DIM, bg); cv.setCursor(150, y); cv.print("R"); }
      cv.setTextColor(stale ? C_DIM : sigColor(t.rssi), bg); cv.setCursor(166, y); cv.printf("%4d", t.rssi);
      int q = constrain((t.rssi + 100) * 38 / 70, 0, 38);
      cv.drawRect(196, y, 40, 7, C_SEL);
      cv.fillRect(197, y + 1, q, 5, stale ? C_DIM : sigColor(t.rssi));
    }
    if (view.empty()) { cv.setTextColor(C_DIM, C_BG); cv.setCursor(80, 60); cv.print("Scanning..."); }
    ui::footer(ble ? "ENTER lock on  TAB WiFi  F filter  X M" : "ENTER lock on  TAB BLE  X clear  M snd");
    ui::push();
  }
  s_locked = false;
  radioStop();
  xSemaphoreTake(s_mux, portMAX_DELAY);
  free(s_list); s_list = nullptr; s_n = 0;
  xSemaphoreGive(s_mux);
}
