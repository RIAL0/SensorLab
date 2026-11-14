// Web mode: the Cardputer becomes a password-protected WiFi access point and serves a browser
// interface for the SD card (browse, download, upload, edit, rename, delete), system information,
// a WiFi scan and the settings. Started from Settings; everything stops when the screen is left.
#include "apps.h"
#include "settings.h"
#include "ui.h"
#include "web_page.h"
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>

static WebServer* s_srv = nullptr;
static uint32_t s_reqs = 0;
static String s_lastReq;
static File s_upFile;
static bool s_upOk = false;

static void hit(const char* what) { s_reqs++; s_lastReq = what; }

// Paths come from the browser: force them absolute and refuse anything that climbs out of the card.
static bool cleanPath(String& p) {
  if (!p.startsWith("/")) p = "/" + p;
  if (p.indexOf("..") >= 0 || p.indexOf('\\') >= 0) return false;
  while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  return true;
}

static String jsonStr(const String& s) {
  String o = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o + "\"";
}

static const char* mimeOf(const String& path) {
  String p = path; p.toLowerCase();
  if (p.endsWith(".wav")) return "audio/wav";
  if (p.endsWith(".png")) return "image/png";
  if (p.endsWith(".jpg") || p.endsWith(".jpeg")) return "image/jpeg";
  if (p.endsWith(".gif")) return "image/gif";
  if (p.endsWith(".bmp")) return "image/bmp";
  if (p.endsWith(".htm") || p.endsWith(".html")) return "text/plain";       // never run pages from the card
  if (filesIsText(p)) return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

static bool removeAll(const String& path, int depth = 0) {
  File f = SD.open(path);
  if (!f) return false;
  bool dir = f.isDirectory();
  if (!dir) { f.close(); return SD.remove(path); }
  if (depth > 8) { f.close(); return false; }
  std::vector<String> kids;
  while (true) {
    File k = f.openNextFile();
    if (!k) break;
    String n = k.name();
    kids.push_back(n.substring(n.lastIndexOf('/') + 1));
    k.close();
  }
  f.close();
  for (auto& k : kids) if (!removeAll(path + "/" + k, depth + 1)) return false;
  return SD.rmdir(path);
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
    default: return "other";
  }
}

// ------------------------------------------------------------------ request handlers
static void hRoot() {
  hit("page");
  s_srv->sendHeader("Content-Encoding", "gzip");
  s_srv->sendHeader("Cache-Control", "no-cache");
  s_srv->send_P(200, "text/html; charset=utf-8", (const char*)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
}

static void hList() {
  hit("list");
  String p = s_srv->arg("path");
  if (!cleanPath(p)) { s_srv->send(400, "text/plain", "bad path"); return; }
  File d = SD.open(p);
  if (!d || !d.isDirectory()) { s_srv->send(404, "text/plain", "not a folder"); return; }
  s_srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  s_srv->send(200, "application/json", "");
  String out = "[";
  bool first = true;
  for (int n = 0; n < 500; n++) {
    File f = d.openNextFile();
    if (!f) break;
    String name = f.name();
    name = name.substring(name.lastIndexOf('/') + 1);
    if (!first) out += ',';
    first = false;
    out += "{\"n\":" + jsonStr(name) + ",\"d\":" + (f.isDirectory() ? "1" : "0") + ",\"s\":" + String((uint32_t)f.size()) + "}";
    f.close();
    if (out.length() > 1200) { s_srv->sendContent(out); out = ""; }
  }
  d.close();
  out += "]";
  s_srv->sendContent(out);
  s_srv->sendContent("");
}

static void hGet() {
  hit("download");
  String p = s_srv->arg("path");
  if (!cleanPath(p)) { s_srv->send(400, "text/plain", "bad path"); return; }
  File f = SD.open(p);
  if (!f || f.isDirectory()) { s_srv->send(404, "text/plain", "not found"); return; }
  if (s_srv->hasArg("dl")) {
    String name = p.substring(p.lastIndexOf('/') + 1);
    name.replace("\"", "");
    s_srv->sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  }
  s_srv->streamFile(f, mimeOf(p));
  f.close();
}

static void hSave() {
  hit("save");
  String p = s_srv->arg("path");
  if (!cleanPath(p) || p == "/") { s_srv->send(400, "text/plain", "bad path"); return; }
  if (s_srv->hasArg("new") && SD.exists(p)) { s_srv->send(409, "text/plain", "That name already exists"); return; }
  String body = s_srv->arg("plain");
  String tmp = p + ".tmp";
  SD.remove(tmp);
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) { s_srv->send(500, "text/plain", "cannot write to the card"); return; }
  bool ok = body.length() == 0 || f.print(body) == body.length();
  f.close();
  if (!ok) { SD.remove(tmp); s_srv->send(500, "text/plain", "write failed (card full?)"); return; }
  SD.remove(p);
  if (!SD.rename(tmp, p)) { s_srv->send(500, "text/plain", "could not replace the file"); return; }
  s_srv->send(200, "text/plain", "ok");
}

static void hDelete() {
  hit("delete");
  String p = s_srv->arg("path");
  if (!cleanPath(p) || p == "/") { s_srv->send(400, "text/plain", "bad path"); return; }
  if (!SD.exists(p)) { s_srv->send(404, "text/plain", "not found"); return; }
  s_srv->send(removeAll(p) ? 200 : 500, "text/plain", "delete");
}

static void hRename() {
  hit("rename");
  String a = s_srv->arg("from"), b = s_srv->arg("to");
  if (!cleanPath(a) || !cleanPath(b) || a == "/" || b == "/") { s_srv->send(400, "text/plain", "bad path"); return; }
  if (SD.exists(b)) { s_srv->send(409, "text/plain", "That name already exists"); return; }
  s_srv->send(SD.rename(a, b) ? 200 : 500, "text/plain", "rename");
}

static void hMkdir() {
  hit("new folder");
  String p = s_srv->arg("path");
  if (!cleanPath(p) || p == "/") { s_srv->send(400, "text/plain", "bad path"); return; }
  if (SD.exists(p)) { s_srv->send(409, "text/plain", "That name already exists"); return; }
  s_srv->send(SD.mkdir(p) ? 200 : 500, "text/plain", "mkdir");
}

static void hUploadDone() {
  hit("upload");
  s_srv->send(s_upOk ? 200 : 500, "text/plain", s_upOk ? "ok" : "upload failed (card full?)");
}

static void hUploadData() {
  HTTPUpload& u = s_srv->upload();
  if (u.status == UPLOAD_FILE_START) {
    s_upOk = false;
    String dir = s_srv->arg("path"), name = u.filename;
    name = name.substring(name.lastIndexOf('/') + 1);
    if (!cleanPath(dir) || !name.length() || name.indexOf("..") >= 0) return;
    if (s_upFile) s_upFile.close();
    s_upFile = SD.open((dir == "/" ? "" : dir) + "/" + name, FILE_WRITE);
    s_upOk = (bool)s_upFile;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (s_upOk && s_upFile.write(u.buf, u.currentSize) != u.currentSize) s_upOk = false;
  } else if (u.status == UPLOAD_FILE_END || u.status == UPLOAD_FILE_ABORTED) {
    if (s_upFile) s_upFile.close();
    if (u.status == UPLOAD_FILE_ABORTED) s_upOk = false;
  }
}

static void hSys() {
  hit("system");
  float v = ui::battVolts();
  char b[640];
  uint8_t m[6];
  WiFi.softAPmacAddress(m);
  snprintf(b, sizeof(b),
           "{\"version\":\"%s\",\"built\":\"%s\",\"board\":\"%s\",\"uptime\":%lu,\"mac\":\"%s\",\"battPct\":%d,\"battV\":%.3f,"
           "\"chip\":\"%s\",\"cores\":%d,\"cpuMHz\":%lu,\"tempC\":%.1f,\"heapFree\":%lu,\"heapMin\":%lu,\"flash\":%lu,\"fwSize\":%lu,"
           "\"sdTotal\":%llu,\"sdUsed\":%llu}",
           SL_VERSION, __DATE__, M5.getBoard() == m5::board_t::board_M5CardputerADV ? "M5Cardputer ADV" : "M5Cardputer",
           (unsigned long)(millis() / 1000), ui::mac(m).c_str(), ui::lipoPct(v), v, ESP.getChipModel(), (int)ESP.getChipCores(),
           (unsigned long)getCpuFrequencyMhz(), temperatureRead(), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
           (unsigned long)ESP.getFlashChipSize(), (unsigned long)ESP.getSketchSize(),
           (unsigned long long)(ui::sdReady() ? SD.totalBytes() : 0), (unsigned long long)(ui::sdReady() ? SD.usedBytes() : 0));
  s_srv->send(200, "application/json", b);
}

static void hSettingsGet() {
  hit("settings");
  char b[120];
  snprintf(b, sizeof(b), "{\"brightness\":%d,\"dim\":%d,\"off\":%d,\"mic\":%d,\"vol\":%d}", cfg.brightness, cfg.dimIdx, cfg.offIdx, cfg.micGain, cfg.volume);
  s_srv->send(200, "application/json", b);
}

static void hSettingsSet() {
  hit("settings change");
  cfg.brightness = constrain(s_srv->arg("b").toInt(), 16, 255);
  cfg.dimIdx = constrain(s_srv->arg("dim").toInt(), 0, 5);
  cfg.offIdx = constrain(s_srv->arg("off").toInt(), 0, 5);
  cfg.micGain = constrain(s_srv->arg("mic").toInt(), 0, 10);
  cfg.volume = constrain(s_srv->arg("vol").toInt(), 0, 255);
  cfgSave();
  ui::stayAwake();                                  // also applies the new brightness
  M5Cardputer.Display.setBrightness(cfg.brightness);
  s_srv->send(200, "text/plain", "ok");
}

// Scan for networks while staying an access point. The browser polls until the result is ready.
static void hScan() {
  hit("wifi scan");
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { s_srv->send(200, "application/json", "{\"state\":\"scanning\"}"); return; }
  if (n < 0) {
    WiFi.scanNetworks(true, true, false, 120);
    s_srv->send(200, "application/json", "{\"state\":\"scanning\"}");
    return;
  }
  String out = "{\"state\":\"done\",\"aps\":[";
  for (int i = 0; i < n && i < 60; i++) {
    if (i) out += ',';
    out += "{\"ssid\":" + jsonStr(WiFi.SSID(i)) + ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\",\"ch\":" + String(WiFi.channel(i)) +
           ",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"auth\":\"" + authName(WiFi.encryptionType(i)) + "\"}";
  }
  out += "]}";
  WiFi.scanDelete();
  s_srv->send(200, "application/json", out);
}

// ------------------------------------------------------------------ the mode itself
static String makePassword() {
  static const char set[] = "abcdefghjkmnpqrstuvwxyz23456789";      // no look-alike characters
  String p;
  for (int i = 0; i < 8; i++) p += set[esp_random() % (sizeof(set) - 1)];
  return p;
}

void appWeb() {
  if (!ui::sdMount()) ui::toast("No SD card: files unavailable", C_WARN, 1400);
  Preferences pr;
  pr.begin("slweb", false);
  String pass = pr.getString("pass", "");
  if (pass.length() < 8) { pass = makePassword(); pr.putString("pass", pass); }

  uint8_t m[6];
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPmacAddress(m);
  char ssid[24];
  snprintf(ssid, sizeof(ssid), "SensorLab-%02X%02X", m[4], m[5]);
  auto apStart = [&]() { return WiFi.softAP(ssid, pass.c_str(), 6, 0, 2); };
  if (!apStart()) { ui::toast("Could not start the access point", C_BAD, 1600); WiFi.mode(WIFI_OFF); return; }

  WebServer srv(80);
  s_srv = &srv;
  s_reqs = 0; s_lastReq = "";
  srv.on("/", HTTP_GET, hRoot);
  srv.on("/api/list", HTTP_GET, hList);
  srv.on("/api/get", HTTP_GET, hGet);
  srv.on("/api/save", HTTP_POST, hSave);
  srv.on("/api/delete", HTTP_POST, hDelete);
  srv.on("/api/rename", HTTP_POST, hRename);
  srv.on("/api/mkdir", HTTP_POST, hMkdir);
  srv.on("/api/upload", HTTP_POST, hUploadDone, hUploadData);
  srv.on("/api/sys", HTTP_GET, hSys);
  srv.on("/api/settings", HTTP_GET, hSettingsGet);
  srv.on("/api/settings", HTTP_POST, hSettingsSet);
  srv.on("/api/scan", HTTP_GET, hScan);
  srv.onNotFound([]() { s_srv->send(404, "text/plain", "no such page"); });
  srv.begin();

  uint32_t lastDraw = 0;
  while (true) {
    srv.handleClient();
    ui::Key k = ui::poll();
    if (k.back || k.enter) {
      if (!WiFi.softAPgetStationNum() || ui::confirm("A device is connected. Stop anyway?")) break;
    }
    if (k.lc == 'r' && ui::confirm("Make a new password?")) {
      pass = makePassword();
      pr.putString("pass", pass);
      WiFi.softAPdisconnect(false);
      apStart();
    }
    if (millis() - lastDraw < 300) { delay(2); continue; }
    lastDraw = millis();
    int clients = WiFi.softAPgetStationNum();
    ui::clear();
    ui::header("Web mode", clients ? "CONNECTED" : "WAITING");
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 2); cv.print("1. Join this WiFi network:");
    cv.setTextColor(C_FG, C_BG); cv.setTextSize(2); cv.setCursor(4, TOP + 12); cv.print(ssid); cv.setTextSize(1);
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 32); cv.print("Password:");
    cv.setTextColor(C_YEL, C_BG); cv.setTextSize(2); cv.setCursor(64, TOP + 29); cv.print(pass); cv.setTextSize(1);
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 50); cv.print("2. Open in a browser:");
    cv.setTextColor(C_OK, C_BG); cv.setTextSize(2); cv.setCursor(4, TOP + 60); cv.print("192.168.4.1"); cv.setTextSize(1);
    cv.setTextColor(clients ? C_OK : C_DIM, C_BG); cv.setCursor(4, TOP + 82);
    cv.printf("%d device%s connected   %lu requests", clients, clients == 1 ? "" : "s", (unsigned long)s_reqs);
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, TOP + 92);
    if (s_lastReq.length()) cv.printf("last: %.30s", s_lastReq.c_str());
    else cv.print("Phone: switch off mobile data if it fails.");
    ui::footer("ENTER stop    R new password");
    ui::push();
  }
  srv.stop();
  s_srv = nullptr;
  if (s_upFile) s_upFile.close();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  pr.end();
  ui::toast("Web mode stopped", C_DIM, 700);
}
