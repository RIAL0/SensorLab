// "Join network": connect to a WiFi network the user has the password for, then report what the
// network reveals from the inside - addressing, link quality, reachability and the devices on it.
// Opened from WiFi Scan with J. Nothing is stored: the password is kept in RAM for this session only.
#include "apps.h"
#include "ui.h"
#include <WiFi.h>
#include <algorithm>
#include <esp_wifi.h>
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "ping/ping_sock.h"

String wifiVendor(const uint8_t* m);   // app_wifi.cpp
void wifiQuietSta();

// ------------------------------------------------------------------ ping
struct PingRes { volatile int recv = 0; volatile uint32_t sum = 0, mn = 0xFFFFFFFF, mx = 0; volatile bool done = false; };

static void pingOk(esp_ping_handle_t h, void* a) {
  PingRes* r = (PingRes*)a;
  uint32_t t = 0;
  esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
  r->recv = r->recv + 1; r->sum = r->sum + t;
  if (t < r->mn) r->mn = t;
  if (t > r->mx) r->mx = t;
}
static void pingEnd(esp_ping_handle_t, void* a) { ((PingRes*)a)->done = true; }

static String pingText(IPAddress ip, int count) {
  PingRes r;
  esp_ping_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.count = count; cfg.interval_ms = 250; cfg.timeout_ms = 800; cfg.data_size = 32;
  cfg.ttl = 64; cfg.task_stack_size = 4096; cfg.task_prio = 2;
  IP_ADDR4(&cfg.target_addr, ip[0], ip[1], ip[2], ip[3]);
  esp_ping_callbacks_t cb;
  cb.cb_args = &r; cb.on_ping_success = pingOk; cb.on_ping_timeout = nullptr; cb.on_ping_end = pingEnd;
  esp_ping_handle_t h;
  if (esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) return "could not start";
  esp_ping_start(h);
  uint32_t until = millis() + count * 1200 + 500;
  while (!r.done && millis() < until) { M5Cardputer.update(); delay(20); }
  esp_ping_stop(h);
  esp_ping_delete_session(h);
  if (!r.recv) return "no reply";
  char b[48];
  snprintf(b, sizeof(b), "%d/%d replies, %lu ms avg", (int)r.recv, count, (unsigned long)(r.sum / r.recv));
  return b;
}

// ------------------------------------------------------------------ ARP sweep
// lwIP's ARP table is small, so hosts are asked a few at a time from inside the network thread.
static const int BATCH = 6;
struct ArpJob {
  uint32_t ip[BATCH];          // host byte order
  int n;
  bool request;                // true: send requests, false: collect answers
  bool found[BATCH];
  uint8_t mac[BATCH][6];
  SemaphoreHandle_t done;
};
static void arpFn(void* ctx) {
  ArpJob* j = (ArpJob*)ctx;
  struct netif* nif = netif_default;
  for (int i = 0; nif && i < j->n; i++) {
    ip4_addr_t a;
    a.addr = lwip_htonl(j->ip[i]);
    if (j->request) {
      etharp_request(nif, &a);
    } else {
      struct eth_addr* eth = nullptr;
      const ip4_addr_t* ipr = nullptr;
      j->found[i] = etharp_find_addr(nif, &a, &eth, &ipr) >= 0 && eth;
      if (j->found[i]) memcpy(j->mac[i], eth->addr, 6);
    }
  }
  xSemaphoreGive(j->done);
}

struct Host { uint32_t ip; uint8_t mac[6]; };

static void findDevices() {
  uint32_t me = ntohl((uint32_t)WiFi.localIP()), mask = ntohl((uint32_t)WiFi.subnetMask()), gw = ntohl((uint32_t)WiFi.gatewayIP());
  uint32_t first = (me & mask) + 1, last = (me | ~mask) - 1;
  if (last - first > 253) { first = (me & 0xFFFFFF00) + 1; last = (me & 0xFFFFFF00) + 254; }   // large networks: this /24 only
  std::vector<Host> hosts;
  ArpJob job;
  job.done = xSemaphoreCreateBinary();
  bool aborted = false;
  for (uint32_t base = first; base <= last && !aborted; base += BATCH) {
    job.n = 0;
    for (uint32_t ip = base; ip <= last && job.n < BATCH; ip++) if (ip != me) job.ip[job.n++] = ip;
    if (!job.n) continue;
    for (int pass = 0; pass < 2; pass++) {             // ask twice: a single ARP request is easily missed
      job.request = true;
      tcpip_callback(arpFn, &job);
      xSemaphoreTake(job.done, pdMS_TO_TICKS(1000));
      uint32_t until = millis() + 110;
      while (millis() < until) { if (ui::poll().back) aborted = true; delay(5); }
    }
    job.request = false;
    memset(job.found, 0, sizeof(job.found));
    tcpip_callback(arpFn, &job);
    xSemaphoreTake(job.done, pdMS_TO_TICKS(1000));
    for (int i = 0; i < job.n; i++) if (job.found[i]) { Host h; h.ip = job.ip[i]; memcpy(h.mac, job.mac[i], 6); hosts.push_back(h); }

    ui::clear();
    ui::header("Finding devices");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(4, 40);
    cv.printf("Asking %s ...", IPAddress(htonl(base)).toString().c_str());
    cv.setCursor(4, 54); cv.printf("%d devices answered so far", (int)hosts.size());
    cv.drawRect(4, 72, SCR_W - 8, 9, C_DIM);
    cv.fillRect(5, 73, (int)((uint64_t)(base - first) * (SCR_W - 10) / max((uint32_t)1, last - first)), 7, C_OK);
    ui::footer("` stop");
    ui::push();
  }
  vSemaphoreDelete(job.done);
  Host self; self.ip = me; WiFi.macAddress(self.mac);
  hosts.push_back(self);
  std::sort(hosts.begin(), hosts.end(), [](const Host& a, const Host& b) { return a.ip < b.ip; });

  // vendor lookups can be slow with a large oui.csv, so resolve them once
  std::vector<String> vend;
  for (auto& h : hosts) vend.push_back(h.ip == me ? String("this Cardputer") : wifiVendor(h.mac));

  ui::pager("Devices on network", [&](std::vector<String>& L) {
    char b[64];
    snprintf(b, sizeof(b), "#%d devices%s", (int)hosts.size(), aborted ? " (scan stopped early)" : "");
    L.push_back(b);
    for (size_t i = 0; i < hosts.size(); i++) {
      auto& h = hosts[i];
      String tag = h.ip == gw ? "  [router]" : h.ip == me ? "  [me]" : "";
      L.push_back(IPAddress(htonl(h.ip)).toString() + tag);
      L.push_back(" \t" + ui::mac(h.mac));
      L.push_back(" \t" + vend[i]);
    }
    L.push_back("#Note");
    L.push_back("Devices that are asleep or block ARP replies do not appear.");
  }, 0, "netdevices");
}

// ------------------------------------------------------------------ details + tests
static const char* authText(int a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2-PSK";
    case WIFI_AUTH_WPA3_PSK: return "WPA3-SAE";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "other";
  }
}

static void details() {
  ui::pager("Connection details", [](std::vector<String>& L) {
    char b[64];
    wifi_ap_record_t ap;
    bool have = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    L.push_back("#Link");
    L.push_back("Network\t" + WiFi.SSID());
    L.push_back("Access point\t" + WiFi.BSSIDstr());
    if (have) {
      snprintf(b, sizeof(b), "Channel\t%d (%d MHz)%s", ap.primary, ap.primary == 14 ? 2484 : 2407 + ap.primary * 5,
               ap.second == WIFI_SECOND_CHAN_NONE ? "" : ", 40 MHz"); L.push_back(b);
      snprintf(b, sizeof(b), "Signal\t%d dBm", ap.rssi); L.push_back(b);
      L.push_back(String("Security\t") + authText(ap.authmode));
      String phy;
      if (ap.phy_11b) phy += "b ";
      if (ap.phy_11g) phy += "g ";
      if (ap.phy_11n) phy += "n ";
      L.push_back("802.11 modes\t" + phy);
    }
    L.push_back("#Addresses");
    L.push_back("My IP\t" + WiFi.localIP().toString());
    snprintf(b, sizeof(b), "Subnet mask\t%s (/%d)", WiFi.subnetMask().toString().c_str(), WiFi.subnetCIDR()); L.push_back(b);
    L.push_back("Network\t" + WiFi.networkID().toString());
    L.push_back("Broadcast\t" + WiFi.broadcastIP().toString());
    L.push_back("Router\t" + WiFi.gatewayIP().toString());
    L.push_back("DNS 1\t" + WiFi.dnsIP(0).toString());
    L.push_back("DNS 2\t" + WiFi.dnsIP(1).toString());
    uint32_t hosts = ~ntohl((uint32_t)WiFi.subnetMask());
    snprintf(b, sizeof(b), "Room for\t%lu devices", (unsigned long)(hosts > 1 ? hosts - 1 : 0)); L.push_back(b);
    L.push_back("#This device");
    L.push_back("MAC\t" + WiFi.macAddress());
    L.push_back(String("Hostname\t") + WiFi.getHostname());
    L.push_back("#Router");
    uint8_t* bs = WiFi.BSSID();
    if (bs) L.push_back("Radio vendor\t" + wifiVendor(bs));
  }, 2000, "netinfo");
}

static void tests() {
  std::vector<String> out;
  auto show = [&](const char* doing) {
    ui::clear(); ui::header("Connectivity test");
    int y = TOP + 2;
    for (auto& l : out) { cv.setTextColor(C_FG, C_BG); cv.setCursor(4, y); cv.print(l); y += 10; }
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, y + 2); cv.print(doing);
    ui::footer("please wait"); ui::push();
  };
  show("Pinging the router...");
  out.push_back("Router: " + pingText(WiFi.gatewayIP(), 4));
  show("Pinging 1.1.1.1 (internet)...");
  String net = pingText(IPAddress(1, 1, 1, 1), 4);
  out.push_back("Internet: " + net);
  show("Looking up example.com (DNS)...");
  IPAddress res;
  uint32_t t0 = millis();
  bool dns = WiFi.hostByName("example.com", res);
  char b[64];
  if (dns) snprintf(b, sizeof(b), "DNS: ok in %lu ms", (unsigned long)(millis() - t0)); else snprintf(b, sizeof(b), "DNS: lookup failed");
  out.push_back(b);
  out.push_back("");
  out.push_back(net == "no reply" && !dns ? "No internet access from this network." : net == "no reply" ? "DNS works; ping to internet blocked." : "Internet reachable.");
  show("");
  ui::footer("any key to continue"); ui::push();
  while (!ui::poll().any) delay(20);
}

// ------------------------------------------------------------------ entry point
void wifiJoin(const uint8_t* bssid, const char* ssidIn, int channel, int authmode) {
  static String lastSsid, lastPass;
  String ssid = ssidIn;
  if (!ssid.length() && !ui::input("Hidden network: enter its name", ssid, 32)) return;
  if (!ssid.length()) return;
  if (authmode == WIFI_AUTH_WPA2_ENTERPRISE) { ui::toast("Enterprise login is not supported", C_WARN, 1600); return; }
  String pass;
  if (authmode != WIFI_AUTH_OPEN) {
    if (ssid == lastSsid) pass = lastPass;
    String title = "Password for " + ssid.substring(0, 20);
    if (!ui::input(title.c_str(), pass, 63)) return;
  }
  WiFi.scanDelete();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr, channel, ssidIn[0] ? bssid : nullptr);
  uint32_t t0 = millis();
  bool cancelled = false;
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    if (ui::poll().back) { cancelled = true; break; }
    ui::clear(); ui::header("Joining network");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(4, 44); cv.printf("%.36s", ssid.c_str());
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, 58); cv.printf("connecting... %lu s", (unsigned long)((millis() - t0) / 1000));
    ui::footer("` cancel"); ui::push();
    delay(60);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false, false);
    wifiQuietSta();
    if (!cancelled) ui::toast("Could not join (wrong password?)", C_BAD, 1800);
    return;
  }
  lastSsid = ssid; lastPass = pass;

  static const char* const opts[] = {"Connection details", "Connectivity test", "Find devices on this network", "Disconnect"};
  int sel = 0;
  uint32_t lastDraw = 0;
  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.up) sel = (sel + 3) % 4;
    if (k.down) sel = (sel + 1) % 4;
    if (k.enter) {
      if (sel == 0) details();
      else if (sel == 1) tests();
      else if (sel == 2) findDevices();
      else break;
    }
    if (millis() - lastDraw < 200 && !k.any) { delay(10); continue; }
    lastDraw = millis();
    bool up = WiFi.status() == WL_CONNECTED;
    ui::clear();
    char right[20]; snprintf(right, sizeof(right), "%d dBm", (int)WiFi.RSSI());
    ui::header(up ? "Connected" : "Connection lost", up ? right : nullptr);
    cv.setTextColor(up ? C_OK : C_BAD, C_BG); cv.setCursor(4, TOP + 2); cv.printf("%.38s", ssid.c_str());
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 12);
    cv.printf("IP %s  router %s", WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str());
    for (int i = 0; i < 4; i++) {
      int y = TOP + 30 + i * 17;
      if (i == sel) cv.fillRoundRect(4, y - 4, SCR_W - 8, 15, 3, C_SEL);
      cv.setTextColor(i == sel ? C_FG : C_DIM, i == sel ? C_SEL : C_BG); cv.setCursor(12, y); cv.print(opts[i]);
    }
    ui::footer(";/. select   ENTER open   ` disconnect");
    ui::push();
  }
  WiFi.disconnect(false, false);
  wifiQuietSta();
  ui::toast("Disconnected", C_DIM, 600);
}
