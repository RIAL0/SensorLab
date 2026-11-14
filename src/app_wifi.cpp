// WiFi: (1) access-point scanner with full per-network detail,
//       (2) passive monitor - receive-only channel activity and device list.
// Nothing here connects to a network or transmits anything other than standard probe requests
// during the active scan (the same thing every phone does when listing networks).
#include "apps.h"
#include "ui.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <algorithm>

// ------------------------------------------------------------------ helpers
struct Oui { uint8_t b[3]; const char* name; };
static const Oui OUIS[] = {
  {{0x24, 0x0A, 0xC4}, "Espressif"}, {{0x30, 0xAE, 0xA4}, "Espressif"}, {{0x7C, 0xDF, 0xA1}, "Espressif"},
  {{0x24, 0x6F, 0x28}, "Espressif"}, {{0x84, 0xCC, 0xA8}, "Espressif"}, {{0xA4, 0xCF, 0x12}, "Espressif"},
  {{0xB8, 0x27, 0xEB}, "Raspberry Pi"}, {{0xDC, 0xA6, 0x32}, "Raspberry Pi"}, {{0xE4, 0x5F, 0x01}, "Raspberry Pi"},
  {{0x24, 0xA4, 0x3C}, "Ubiquiti"}, {{0xFC, 0xEC, 0xDA}, "Ubiquiti"}, {{0x78, 0x8A, 0x20}, "Ubiquiti"},
  {{0x80, 0x2A, 0xA8}, "Ubiquiti"}, {{0x50, 0xC7, 0xBF}, "TP-Link"}, {{0xF4, 0xF5, 0xD8}, "Google"},
  {{0x44, 0x65, 0x0D}, "Amazon"}, {{0x00, 0x0E, 0x58}, "Sonos"}, {{0x00, 0xE0, 0xFC}, "Huawei"},
  {{0x00, 0x1B, 0x21}, "Intel"}, {{0x64, 0x09, 0x80}, "Xiaomi"}, {{0x28, 0x6C, 0x07}, "Xiaomi"},
  {{0x00, 0x1C, 0xB3}, "Apple"}, {{0xF0, 0x18, 0x98}, "Apple"}, {{0xAC, 0xBC, 0x32}, "Apple"},
};

// Vendor lookup: optional /SensorLab/oui.csv on the SD card ("AABBCC,Vendor name" per line),
// then a small built-in table.
String wifiVendor(const uint8_t* m) {
  if (m[0] & 0x02) return "randomized / locally administered";
  if (ui::sdReady() && SD.exists("/SensorLab/oui.csv")) {
    char want[7];
    snprintf(want, sizeof(want), "%02X%02X%02X", m[0], m[1], m[2]);
    File f = SD.open("/SensorLab/oui.csv");
    while (f && f.available()) {
      String l = f.readStringUntil('\n');
      if (l.length() > 7 && l.substring(0, 6).equalsIgnoreCase(want)) { f.close(); l.trim(); return l.substring(7); }
    }
    if (f) f.close();
  }
  for (auto& o : OUIS) if (!memcmp(o.b, m, 3)) return o.name;
  return "unknown (add oui.csv to SD)";
}

static const char* authName(int a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WAPI_PSK: return "WAPI";
    default: return "other";
  }
}
static const char* authShort(int a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP ";
    case WIFI_AUTH_WPA_PSK: return "WPA ";
    case WIFI_AUTH_WPA2_PSK: case WIFI_AUTH_WPA_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "ENT ";
    case WIFI_AUTH_WPA3_PSK: case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA3";
    default: return "?   ";
  }
}
static const char* cipherName(int c) {
  switch (c) {
    case WIFI_CIPHER_TYPE_NONE: return "none";
    case WIFI_CIPHER_TYPE_WEP40: return "WEP40";
    case WIFI_CIPHER_TYPE_WEP104: return "WEP104";
    case WIFI_CIPHER_TYPE_TKIP: return "TKIP";
    case WIFI_CIPHER_TYPE_CCMP: return "CCMP (AES)";
    case WIFI_CIPHER_TYPE_TKIP_CCMP: return "TKIP+CCMP";
    case WIFI_CIPHER_TYPE_AES_CMAC128: return "AES-CMAC-128";
    case WIFI_CIPHER_TYPE_SMS4: return "SMS4";
    case WIFI_CIPHER_TYPE_GCMP: return "GCMP";
    case WIFI_CIPHER_TYPE_GCMP256: return "GCMP-256";
    default: return "unknown";
  }
}
static int chanFreq(int ch) { return ch == 14 ? 2484 : 2407 + ch * 5; }
static float estDist(int rssi, int freqMhz) {      // free-space estimate, order of magnitude only
  return powf(10.0f, (27.55f - 20.0f * log10f(freqMhz) - rssi) / 20.0f);
}
static uint16_t rssiColor(int r) { return r > -60 ? C_OK : r > -75 ? C_YEL : r > -85 ? C_WARN : C_BAD; }
static void rssiBar(int x, int y, int rssi) {
  int q = constrain((rssi + 95) * 30 / 60, 0, 30);
  cv.drawRect(x, y, 32, 7, C_SEL);
  cv.fillRect(x + 1, y + 1, q, 5, rssiColor(rssi));
}

// ------------------------------------------------------------------ scanner
static void apDetail(const wifi_ap_record_t& r) {
  String vendor = wifiVendor(r.bssid);
  ui::pager("WiFi network detail", [&](std::vector<String>& L) {
    char b[64];
    L.push_back("#Identity");
    L.push_back(String("SSID\t") + (r.ssid[0] ? (const char*)r.ssid : "<hidden>"));
    L.push_back("BSSID\t" + ui::mac(r.bssid));
    L.push_back("Vendor\t" + vendor);
    L.push_back("#Radio");
    snprintf(b, sizeof(b), "Channel\t%d (%d MHz)", r.primary, chanFreq(r.primary)); L.push_back(b);
    L.push_back(String("Width\t") + (r.second == WIFI_SECOND_CHAN_ABOVE ? "40 MHz (HT40+)" : r.second == WIFI_SECOND_CHAN_BELOW ? "40 MHz (HT40-)" : "20 MHz"));
    snprintf(b, sizeof(b), "Signal\t%d dBm (%d%%)", r.rssi, constrain(2 * (r.rssi + 100), 0, 100)); L.push_back(b);
    snprintf(b, sizeof(b), "Distance\t~%.0f m (rough)", estDist(r.rssi, chanFreq(r.primary))); L.push_back(b);
    String phy;
    if (r.phy_11b) phy += "b ";
    if (r.phy_11g) phy += "g ";
    if (r.phy_11n) phy += "n ";
    if (r.phy_lr) phy += "LR ";
    L.push_back("802.11 modes\t" + phy);
    snprintf(b, sizeof(b), "Antenna\t%d", r.ant); L.push_back(b);
    L.push_back("#Security");
    L.push_back(String("Auth\t") + authName(r.authmode));
    L.push_back(String("Pairwise\t") + cipherName(r.pairwise_cipher));
    L.push_back(String("Group\t") + cipherName(r.group_cipher));
    L.push_back(String("WPS\t") + (r.wps ? "enabled" : "no"));
    L.push_back("#Other");
    L.push_back(String("FTM responder\t") + (r.ftm_responder ? "yes" : "no"));
    L.push_back(String("FTM initiator\t") + (r.ftm_initiator ? "yes" : "no"));
    if (r.country.cc[0] >= 'A' && r.country.cc[0] <= 'Z') {
      snprintf(b, sizeof(b), "Country\t%c%c  ch %d-%d", r.country.cc[0], r.country.cc[1], r.country.schan, r.country.schan + r.country.nchan - 1); L.push_back(b);
      snprintf(b, sizeof(b), "Max TX power\t%d dBm", r.country.max_tx_power); L.push_back(b);
    } else {
      L.push_back("Country\tnot advertised");
    }
  }, 0, "wifi_ap");
}

// Bring the radio up as a station that never tries to join anything. Launcher may have left
// saved credentials in flash; a background (re)connect attempt aborts scans, so the station
// config is blanked in RAM only (the saved credentials are not erased).
void wifiQuietSta() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  esp_wifi_disconnect();
  wifi_config_t empty = {};
  esp_wifi_set_config(WIFI_IF_STA, &empty);
  esp_wifi_set_ps(WIFI_PS_NONE);
  delay(50);
}

struct ApEnt { wifi_ap_record_t r; uint32_t seen; };

void appWifiScan() {
  wifiQuietSta();
  // The list is cumulative: every channel is scanned on its own and merged in by BSSID, so one
  // short or failed scan cannot blank the list. Entries drop out 45 s after they were last heard.
  std::vector<ApEnt> aps;
  const uint32_t AGE_MS = 45000;
  int sel = 0, top = 0, view = 0, ch = 1, sweeps = 0, fails = 0;
  bool scanning = false, autoScan = true, passive = false;
  uint32_t lastDraw = 0, retryAt = 0, startedAt = 0;
  auto startScan = [&]() {
    WiFi.scanDelete();
    int16_t r = WiFi.scanNetworks(true, true, passive, passive ? 360 : 240, ch);
    scanning = (r == WIFI_SCAN_RUNNING);
    startedAt = millis();
    if (!scanning) { fails++; retryAt = millis() + 150; }
  };
  auto nextChannel = [&]() {
    ch++;
    if (ch > 13) { ch = 1; sweeps++; }
  };
  auto sortAps = [&]() {
    String keep = sel < (int)aps.size() ? ui::mac(aps[sel].r.bssid) : String();
    std::stable_sort(aps.begin(), aps.end(), [](const ApEnt& a, const ApEnt& b) { return a.r.rssi > b.r.rssi; });
    for (int i = 0; i < (int)aps.size(); i++) if (ui::mac(aps[i].r.bssid) == keep) sel = i;
    if (sel >= (int)aps.size()) sel = max(0, (int)aps.size() - 1);
  };
  startScan();

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.up && sel > 0) sel--;
    if (k.down && sel + 1 < (int)aps.size()) sel++;
    if (k.left || k.right || k.lc == 'c') view ^= 1;
    if (k.lc == 'a') { autoScan = !autoScan; if (autoScan && !scanning) retryAt = millis(); }
    if (k.lc == 'p') passive = !passive;
    if (k.lc == 'x') { aps.clear(); sel = top = 0; }
    if (k.lc == 'r' && !scanning && !autoScan) { ch = 1; startScan(); }
    if (k.enter && sel < (int)aps.size()) apDetail(aps[sel].r);
    if (k.lc == 'j' && sel < (int)aps.size()) {
      wifi_ap_record_t r = aps[sel].r;
      wifiJoin(r.bssid, (const char*)r.ssid, r.primary, r.authmode);
      scanning = false; retryAt = millis() + 300;        // back to scanning
    }
    if (k.lc == 's' && !aps.empty()) {
      if (!ui::sdMount()) ui::toast("No SD card", C_BAD);
      else {
        String p = ui::nextFile("wifi", "csv");
        File f = SD.open(p, FILE_WRITE);
        if (f) {
          f.println("ssid,bssid,channel,second,rssi,auth,pairwise,group,11b,11g,11n,wps,country");
          for (auto& e : aps) {
            auto& r = e.r;
            f.printf("\"%s\",%s,%d,%d,%d,%s,%s,%s,%d,%d,%d,%d,%c%c\n", (const char*)r.ssid, ui::mac(r.bssid).c_str(), r.primary, r.second, r.rssi,
                     authName(r.authmode), cipherName(r.pairwise_cipher), cipherName(r.group_cipher), r.phy_11b, r.phy_11g, r.phy_11n, r.wps,
                     r.country.cc[0] >= 'A' ? r.country.cc[0] : '-', r.country.cc[0] >= 'A' ? r.country.cc[1] : '-');
          }
          f.close();
          ui::toast("Saved " + p.substring(11));
        } else ui::toast("SD write failed", C_BAD);
      }
    }

    if (scanning) {
      int n = WiFi.scanComplete();
      if (n >= 0) {
        uint32_t now = millis();
        for (int i = 0; i < n; i++) {
          wifi_ap_record_t* r = (wifi_ap_record_t*)WiFi.getScanInfoByIndex(i);
          if (!r) continue;
          bool hit = false;
          for (auto& e : aps) if (!memcmp(e.r.bssid, r->bssid, 6)) { e.r = *r; e.seen = now; hit = true; break; }
          if (!hit && aps.size() < 120) aps.push_back({*r, now});
        }
        WiFi.scanDelete();
        // a scan that "finishes" almost instantly did not really listen: count it as a failure
        if (now - startedAt < 60) fails++;
        aps.erase(std::remove_if(aps.begin(), aps.end(), [&](const ApEnt& e) { return now - e.seen > AGE_MS; }), aps.end());
        sortAps();
        scanning = false;
        nextChannel();
        retryAt = now + 20;
        if (!autoScan && ch == 1) retryAt = 0;
      } else if (n == WIFI_SCAN_FAILED) {
        scanning = false;
        fails++;
        nextChannel();
        retryAt = millis() + 150;
      }
    } else if (retryAt && millis() >= retryAt && (autoScan || ch != 1)) {
      retryAt = 0;
      startScan();
    }
    if (millis() - lastDraw < 80) { delay(5); continue; }
    lastDraw = millis();

    ui::clear();
    char right[28];
    snprintf(right, sizeof(right), "%d APs ch%d%s", (int)aps.size(), ch, !scanning && !autoScan ? " idle" : passive ? " pas" : "");
    ui::header(view == 0 ? "WiFi Scan" : "WiFi Channels", right);
    if (view == 0) {
      const int rows = ROWS;
      if (sel < top) top = sel;
      if (sel >= top + rows) top = sel - rows + 1;
      if (top < 0) top = 0;
      for (int r = 0; r < rows && top + r < (int)aps.size(); r++) {
        auto& a = aps[top + r].r;
        bool stale = millis() - aps[top + r].seen > 15000;
        int y = TOP + r * LINE_H;
        uint16_t bg = (top + r == sel) ? C_SEL : C_BG;
        if (top + r == sel) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
        char ss[20];
        snprintf(ss, sizeof(ss), "%-18.18s", a.ssid[0] ? (const char*)a.ssid : "<hidden>");
        cv.setTextColor(stale || !a.ssid[0] ? C_DIM : C_FG, bg); cv.setCursor(2, y); cv.print(ss);
        cv.setTextColor(a.authmode == WIFI_AUTH_OPEN ? C_BAD : a.authmode == WIFI_AUTH_WEP ? C_WARN : C_DIM, bg);
        cv.setCursor(114, y); cv.print(authShort(a.authmode));
        cv.setTextColor(C_ACC, bg); cv.setCursor(142, y); cv.printf("%2d", a.primary);
        cv.setTextColor(stale ? C_DIM : rssiColor(a.rssi), bg); cv.setCursor(160, y); cv.printf("%4d", a.rssi);
        rssiBar(190, y, a.rssi);
      }
      if (aps.empty()) {
        cv.setTextColor(C_DIM, C_BG); cv.setCursor(40, 54);
        cv.print(sweeps ? "No networks heard yet" : "Scanning channels 1-13...");
        cv.setCursor(40, 68); cv.printf("sweeps %d  failed scans %d", sweeps, fails);
        if (sweeps >= 2) { cv.setCursor(40, 82); cv.print("Try P for passive scan"); }
      }
      ui::footer("ENTER detail  J join  C chan  S save");
    } else {
      // networks per channel + strongest signal on each
      int cnt[15] = {0}, best[15];
      for (int i = 0; i < 15; i++) best[i] = -100;
      int mx = 1;
      for (auto& e : aps) { auto& a = e.r; if (a.primary >= 1 && a.primary <= 14) { cnt[a.primary]++; if (a.rssi > best[a.primary]) best[a.primary] = a.rssi; if (cnt[a.primary] > mx) mx = cnt[a.primary]; } }
      const int gy = TOP + 12, gh = 72, bw = 16;
      int least = 1;
      for (int c = 1; c <= 13; c++) if (cnt[c] < cnt[least]) least = c;
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, TOP + 1);
      cv.printf("sweeps %d  failed %d  %s scan", sweeps, fails, passive ? "passive" : "active");
      for (int c = 1; c <= 14; c++) {
        int x = 6 + (c - 1) * bw, h = cnt[c] * gh / mx;
        cv.fillRect(x, gy + gh - h, bw - 3, h, cnt[c] ? rssiColor(best[c]) : C_SEL);
        cv.setTextColor(c == ch ? C_YEL : C_FG, C_BG); cv.setCursor(x + (c < 10 ? 3 : 0), gy + gh + 3); cv.print(c);
        if (cnt[c]) { cv.setTextColor(C_FG); cv.setCursor(x + 3, gy + gh - h - 9 < gy ? gy + 1 : gy + gh - h - 9); cv.print(cnt[c]); }
      }
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, gy + gh + 14);
      cv.printf("APs per channel, colour = best RSSI");
      ui::footer("C list A auto P pasv X clr");
      cv.setTextColor(C_OK, C_HDR); cv.setCursor(196, SCR_H - 9); cv.printf("free:%d", least);
    }
    ui::push();
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
}

// ------------------------------------------------------------------ passive monitor
struct Dev {
  uint8_t mac[6];
  uint8_t bssid[6];       // AP this station was seen talking to (all zero = unknown)
  char ssid[33];          // beacon SSID (AP) or last probed SSID (station)
  int8_t rssi;
  uint8_t ch;
  uint8_t flags;          // 1 = AP, 2 = station, 4 = sent probe request
  uint32_t pkts, last;
};
static const int MAXDEV = 96;
static Dev* s_dev = nullptr;
static volatile int s_ndev = 0;
static volatile uint32_t s_chMgmt[15], s_chData[15], s_chCtrl[15], s_total, s_dropped;
static volatile int32_t s_chRssi[15];
static volatile uint8_t s_curCh = 1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static Dev* devFind(const uint8_t* mac) {
  for (int i = 0; i < s_ndev; i++) if (!memcmp(s_dev[i].mac, mac, 6)) return &s_dev[i];
  if (s_ndev >= MAXDEV) { s_dropped++; return nullptr; }
  Dev* d = &s_dev[s_ndev];
  memset(d, 0, sizeof(Dev));
  memcpy(d->mac, mac, 6);
  s_ndev = s_ndev + 1;
  return d;
}

static void IRAM_ATTR sniff(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* p = (const wifi_promiscuous_pkt_t*)buf;
  const uint8_t* f = p->payload;
  int len = p->rx_ctrl.sig_len;
  uint8_t ch = s_curCh;
  if (ch < 1 || ch > 14) return;
  portENTER_CRITICAL(&s_mux);
  s_total++;
  s_chRssi[ch] += p->rx_ctrl.rssi;
  if (type == WIFI_PKT_MGMT) s_chMgmt[ch]++; else if (type == WIFI_PKT_DATA) s_chData[ch]++; else s_chCtrl[ch]++;
  if ((type == WIFI_PKT_MGMT || type == WIFI_PKT_DATA) && len >= 24 && s_dev) {
    uint8_t sub = (f[0] >> 4) & 0x0F, ds = f[1] & 0x03;
    const uint8_t* a1 = f + 4; const uint8_t* a2 = f + 10; const uint8_t* a3 = f + 16;
    if (!(a2[0] & 0x01)) {              // ignore group addresses as transmitter
      Dev* d = devFind(a2);
      if (d) {
        d->rssi = p->rx_ctrl.rssi; d->ch = ch; d->pkts++; d->last = millis();
        int ie = -1;
        if (type == WIFI_PKT_MGMT) {
          if (sub == 8 || sub == 5) { d->flags |= 1; ie = 36; }                 // beacon / probe response
          else if (sub == 4) { d->flags |= 2 | 4; ie = 24; }                    // probe request
          else if (sub == 0 || sub == 2 || sub == 11) { d->flags |= 2; memcpy(d->bssid, a3, 6); }
          if (ie > 0 && len >= ie + 2 && f[ie] == 0) {
            int sl = f[ie + 1];
            if (sl > 0 && sl <= 32 && len >= ie + 2 + sl) { memcpy(d->ssid, f + ie + 2, sl); d->ssid[sl] = 0; }
          }
        } else {
          if (ds == 2) { d->flags |= 1; }                                        // from AP
          else if (ds == 1) { d->flags |= 2; memcpy(d->bssid, a1, 6); }          // station -> AP
        }
      }
    }
  }
  portEXIT_CRITICAL(&s_mux);
}

static bool printable(const char* s) {
  for (; *s; s++) if ((uint8_t)*s < 32 || (uint8_t)*s > 126) return false;
  return true;
}

static void devDetail(const Dev& d, const std::vector<Dev>& all) {
  String vendor = wifiVendor(d.mac);
  String apName;
  for (auto& o : all) if (!memcmp(o.mac, d.bssid, 6) && (o.flags & 1)) apName = o.ssid;
  ui::pager("WiFi device detail", [&](std::vector<String>& L) {
    char b[64];
    L.push_back("MAC\t" + ui::mac(d.mac));
    L.push_back("Vendor\t" + vendor);
    String role;
    if (d.flags & 1) role += "access point ";
    if (d.flags & 2) role += "client station ";
    if (!d.flags) role = "unknown";
    L.push_back("Role\t" + role);
    if (d.flags & 1) L.push_back(String("SSID\t") + (d.ssid[0] ? d.ssid : "<hidden>"));
    if ((d.flags & 4) && !(d.flags & 1)) L.push_back(String("Probed SSID\t") + (d.ssid[0] ? d.ssid : "<broadcast>"));
    static const uint8_t zero[6] = {0};
    if (memcmp(d.bssid, zero, 6)) {
      L.push_back("Talking to AP\t" + ui::mac(d.bssid));
      if (apName.length()) L.push_back("AP name\t" + apName);
    }
    snprintf(b, sizeof(b), "Channel\t%d (%d MHz)", d.ch, chanFreq(d.ch)); L.push_back(b);
    snprintf(b, sizeof(b), "Signal\t%d dBm", d.rssi); L.push_back(b);
    snprintf(b, sizeof(b), "Distance\t~%.0f m (rough)", estDist(d.rssi, chanFreq(d.ch))); L.push_back(b);
    snprintf(b, sizeof(b), "Frames seen\t%lu", (unsigned long)d.pkts); L.push_back(b);
    snprintf(b, sizeof(b), "Last seen\t%lu s ago", (unsigned long)((millis() - d.last) / 1000)); L.push_back(b);
  }, 0, "wifi_dev");
}

void appWifiMonitor() {
  s_dev = (Dev*)calloc(MAXDEV, sizeof(Dev));
  if (!s_dev) { ui::toast("Out of memory", C_BAD); return; }
  s_ndev = 0; s_total = 0; s_dropped = 0;
  for (int i = 0; i < 15; i++) { s_chMgmt[i] = s_chData[i] = s_chCtrl[i] = 0; s_chRssi[i] = 0; }
  wifiQuietSta();
  wifi_promiscuous_filter_t flt = {.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_CTRL};
  esp_wifi_set_promiscuous_filter(&flt);
  wifi_promiscuous_filter_t cflt = {.filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL};
  esp_wifi_set_promiscuous_ctrl_filter(&cflt);
  esp_wifi_set_promiscuous_rx_cb(&sniff);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

  bool hop = true;
  int ch = 1, view = 0, sel = 0, top = 0, filter = 0;
  uint32_t hopAt = millis(), lastDraw = 0, ppsAt = millis(), lastTotal = 0, pps = 0;
  std::vector<Dev> snap;

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.lc == 'h') hop = !hop;
    if (k.lc == 'v' || k.tab) view ^= 1;
    if (k.lc == 'f') { filter = (filter + 1) % 3; sel = 0; }
    if (view == 0 || !hop) {
      if (k.left || k.right) {
        hop = false;
        ch += k.right ? 1 : -1;
        if (ch < 1) ch = 13;
        if (ch > 13) ch = 1;
        s_curCh = ch;
        esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      }
    }
    if (view == 1) {
      if (k.up && sel > 0) sel--;
      if (k.down && sel + 1 < (int)snap.size()) sel++;
      if (k.enter && sel < (int)snap.size()) devDetail(snap[sel], snap);
    }
    if (k.lc == 'x') {
      portENTER_CRITICAL(&s_mux);
      s_ndev = 0; s_total = 0;
      for (int i = 0; i < 15; i++) { s_chMgmt[i] = s_chData[i] = s_chCtrl[i] = 0; s_chRssi[i] = 0; }
      portEXIT_CRITICAL(&s_mux);
      lastTotal = 0; sel = 0;
    }
    if (k.lc == 's') {
      if (!ui::sdMount()) ui::toast("No SD card", C_BAD);
      else {
        String p = ui::nextFile("wifimon", "csv");
        File f = SD.open(p, FILE_WRITE);
        if (f) {
          f.println("mac,role,ssid,associated_bssid,channel,rssi,frames");
          for (auto& d : snap)
            f.printf("%s,%s%s,\"%s\",%s,%d,%d,%lu\n", ui::mac(d.mac).c_str(), (d.flags & 1) ? "AP" : "", (d.flags & 2) ? "STA" : "",
                     printable(d.ssid) ? d.ssid : "", ui::mac(d.bssid).c_str(), d.ch, d.rssi, (unsigned long)d.pkts);
          f.println();
          f.println("channel,mgmt,data,ctrl");
          for (int c = 1; c <= 13; c++) f.printf("%d,%lu,%lu,%lu\n", c, (unsigned long)s_chMgmt[c], (unsigned long)s_chData[c], (unsigned long)s_chCtrl[c]);
          f.close();
          ui::toast("Saved " + p.substring(11));
        } else ui::toast("SD write failed", C_BAD);
      }
    }
    if (hop && millis() - hopAt > 250) {
      hopAt = millis();
      ch = ch % 13 + 1;
      s_curCh = ch;
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    }
    if (millis() - ppsAt >= 1000) { uint32_t t = s_total; pps = t - lastTotal; lastTotal = t; ppsAt = millis(); }
    if (millis() - lastDraw < 100) { delay(5); continue; }
    lastDraw = millis();

    // snapshot the device table, filtered and sorted
    snap.clear();
    portENTER_CRITICAL(&s_mux);
    int n = s_ndev;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < n; i++) {
      Dev d;
      portENTER_CRITICAL(&s_mux);
      d = s_dev[i];
      portEXIT_CRITICAL(&s_mux);
      if (filter == 1 && !(d.flags & 1)) continue;
      if (filter == 2 && (d.flags & 1)) continue;
      snap.push_back(d);
    }
    std::sort(snap.begin(), snap.end(), [](const Dev& a, const Dev& b) { return a.rssi > b.rssi; });
    if (sel >= (int)snap.size()) sel = max(0, (int)snap.size() - 1);

    ui::clear();
    char right[28];
    snprintf(right, sizeof(right), "ch%2d%s %lu/s", ch, hop ? " hop" : " fix", (unsigned long)pps);
    ui::header(view == 0 ? "WiFi Monitor" : filter == 0 ? "Devices: all" : filter == 1 ? "Devices: APs" : "Devices: clients", right);
    if (view == 0) {
      uint32_t mx = 1;
      for (int c = 1; c <= 13; c++) { uint32_t t = s_chMgmt[c] + s_chData[c] + s_chCtrl[c]; if (t > mx) mx = t; }
      const int gy = TOP + 12, gh = 70, bw = 17;
      cv.setTextColor(C_DIM, C_BG); cv.setCursor(2, TOP + 1);
      cv.printf("frames %lu  devices %d%s", (unsigned long)s_total, n, s_dropped ? " (table full)" : "");
      for (int c = 1; c <= 13; c++) {
        int x = 8 + (c - 1) * bw;
        int hm = s_chMgmt[c] * gh / mx, hd = s_chData[c] * gh / mx, hc = s_chCtrl[c] * gh / mx;
        int y = gy + gh;
        cv.fillRect(x, y - hm, bw - 4, hm, C_ACC); y -= hm;
        cv.fillRect(x, y - hd, bw - 4, hd, C_OK); y -= hd;
        cv.fillRect(x, y - hc, bw - 4, hc, C_WARN);
        cv.setTextColor(c == ch ? C_YEL : C_FG, C_BG);
        cv.setCursor(x + (c < 10 ? 3 : 0), gy + gh + 3); cv.print(c);
        if (c == ch) cv.drawFastHLine(x, gy + gh + 12, bw - 4, C_YEL);
      }
      cv.setCursor(2, gy + gh + 15);
      cv.setTextColor(C_ACC, C_BG); cv.print("mgmt ");
      cv.setTextColor(C_OK, C_BG); cv.print("data ");
      cv.setTextColor(C_WARN, C_BG); cv.print("ctrl");
      ui::footer("TAB devices  H hop  ,// chan  X clear");
    } else {
      if (sel < top) top = sel;
      if (sel >= top + ROWS) top = sel - ROWS + 1;
      for (int r = 0; r < ROWS && top + r < (int)snap.size(); r++) {
        auto& d = snap[top + r];
        int y = TOP + r * LINE_H;
        uint16_t bg = (top + r == sel) ? C_SEL : C_BG;
        if (top + r == sel) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
        cv.setTextColor((d.flags & 1) ? C_ACC : C_OK, bg); cv.setCursor(2, y); cv.print((d.flags & 1) ? "AP " : (d.flags & 2) ? "STA" : "?  ");
        char nm[20];
        if (d.ssid[0] && printable(d.ssid)) snprintf(nm, sizeof(nm), "%-17.17s", d.ssid);
        else snprintf(nm, sizeof(nm), "%s", ui::mac(d.mac).c_str());
        cv.setTextColor(C_FG, bg); cv.setCursor(24, y); cv.print(nm);
        cv.setTextColor(C_DIM, bg); cv.setCursor(132, y); cv.printf("%2d", d.ch);
        cv.setTextColor(rssiColor(d.rssi), bg); cv.setCursor(150, y); cv.printf("%4d", d.rssi);
        cv.setTextColor(C_DIM, bg); cv.setCursor(180, y); cv.printf("%6lu", (unsigned long)d.pkts);
      }
      if (snap.empty()) { cv.setTextColor(C_DIM, C_BG); cv.setCursor(70, 60); cv.print("Listening..."); }
      ui::footer("ENTER detail TAB chart F filter S save");
    }
    ui::push();
  }
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  WiFi.mode(WIFI_OFF);
  portENTER_CRITICAL(&s_mux);
  Dev* tmp = s_dev;
  s_dev = nullptr;
  s_ndev = 0;
  portEXIT_CRITICAL(&s_mux);
  free(tmp);
}
