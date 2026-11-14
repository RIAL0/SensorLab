// BLE scanner: everything a device broadcasts in its advertisements and scan responses.
// Scanning is passive listening; connecting to a device only happens when the user asks (G).
#include "apps.h"
#include "ui.h"
#include <NimBLEDevice.h>
#include <algorithm>

struct BleDev {
  uint8_t addr[6];          // little-endian, [5] is the most significant byte
  uint8_t addrType;
  int8_t rssi, rssiMin, rssiMax;
  int8_t txPower;
  bool hasTx, connectable, hasAppearance;
  uint8_t advType, flags;
  uint16_t appearance;
  char name[30];
  uint8_t mfg[40]; uint8_t mfgLen;
  char svc[80];             // advertised service UUIDs, space separated
  uint8_t svcData[32]; uint8_t svcDataLen; char svcDataUuid[40];
  uint8_t raw[62]; uint8_t rawLen;
  uint32_t count, first, last, intervalMs;
};
static const int MAXBLE = 64;
static BleDev* s_b = nullptr;
static int s_nb = 0;
static uint32_t s_advTotal = 0;
static SemaphoreHandle_t s_lock = nullptr;

static const char* companyName(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x0059: return "Nordic Semiconductor";
    case 0x02E5: return "Espressif";
    case 0x0087: return "Garmin";
    case 0x000D: return "Texas Instruments";
    case 0x000F: return "Broadcom";
    case 0x0002: return "Intel";
    case 0x001D: return "Qualcomm";
    case 0x0171: return "Amazon";
    case 0x038F: return "Xiaomi";
    case 0x0499: return "Ruuvi";
    case 0x012D: return "Sony";
    case 0x009E: return "Bose";
    case 0x0157: return "Huami";
    default: return nullptr;
  }
}
static const char* appleType(uint8_t t) {
  switch (t) {
    case 0x02: return "iBeacon";
    case 0x05: return "AirDrop";
    case 0x07: return "AirPods / proximity pairing";
    case 0x09: return "AirPlay target";
    case 0x0A: return "AirPlay source";
    case 0x0C: return "Handoff";
    case 0x0F: return "Nearby Action";
    case 0x10: return "Nearby Info";
    case 0x12: return "Find My";
    default: return nullptr;
  }
}
static const char* svcName(const String& u) {
  struct S { const char* id; const char* n; };
  static const S T[] = {
    {"0x1800", "Generic Access"}, {"0x1801", "Generic Attribute"}, {"0x180a", "Device Information"},
    {"0x180d", "Heart Rate"}, {"0x180f", "Battery"}, {"0x1812", "HID (keyboard/mouse)"},
    {"0x181a", "Environmental Sensing"}, {"0x1816", "Cycling Speed/Cadence"}, {"0x1818", "Cycling Power"},
    {"0x1809", "Health Thermometer"}, {"0x1810", "Blood Pressure"}, {"0x181c", "User Data"},
    {"0xfe2c", "Google Fast Pair"}, {"0xfd6f", "Exposure Notification"}, {"0xfeaa", "Eddystone"},
    {"0xfe95", "Xiaomi"}, {"0xfeed", "Tile"}, {"0xfd5a", "Samsung SmartTag"}, {"0xfee0", "Mi Band / Amazfit"},
    {"0x1826", "Fitness Machine"}, {"0x181d", "Weight Scale"}, {"0x1814", "Running Speed/Cadence"},
    {"0x184e", "LE Audio"}, {"0x1808", "Glucose"}, {"0x1822", "Pulse Oximeter"},
  };
  for (auto& s : T) if (u.equalsIgnoreCase(s.id)) return s.n;
  return nullptr;
}
static const char* appearanceName(uint16_t a) {
  switch (a >> 6) {
    case 1: return "Phone";
    case 2: return "Computer";
    case 3: return "Watch";
    case 4: return "Clock";
    case 5: return "Display";
    case 6: return "Remote control";
    case 7: return "Eye glasses";
    case 8: return "Tag";
    case 9: return "Keyring";
    case 10: return "Media player";
    case 11: return "Barcode scanner";
    case 12: return "Thermometer";
    case 13: return "Heart rate sensor";
    case 14: return "Blood pressure";
    case 15: return "HID (keyboard/mouse/gamepad)";
    case 16: return "Glucose meter";
    case 17: return "Running/walking sensor";
    case 18: return "Cycling";
    default: return "other";
  }
}
static const char* addrTypeName(const BleDev& d) {
  if (d.addrType == 0) return "public (fixed, has vendor OUI)";
  if (d.addrType == 1) {
    uint8_t top = d.addr[5] >> 6;
    return top == 3 ? "random static" : top == 1 ? "resolvable private (rotates)" : top == 0 ? "non-resolvable private" : "random";
  }
  return d.addrType == 2 ? "public identity" : "random identity";
}
static const char* advTypeName(uint8_t t) {
  switch (t) {
    case 0: return "ADV_IND (connectable + scannable)";
    case 1: return "ADV_DIRECT_IND (directed)";
    case 2: return "ADV_SCAN_IND (scannable only)";
    case 3: return "ADV_NONCONN_IND (beacon)";
    case 4: return "seen only as scan response";
    default: return "extended";
  }
}
static String addrStr(const BleDev& d) {
  char b[20];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", d.addr[5], d.addr[4], d.addr[3], d.addr[2], d.addr[1], d.addr[0]);
  return b;
}
String wifiVendor(const uint8_t* m);   // shared OUI lookup (app_wifi.cpp)

// ---- decoders for model information that devices broadcast without being connected to.
// The Apple model codes come from community-maintained lists and may be incomplete.
static const char* appleAudioModel(uint16_t m) {
  switch (m) {
    case 0x0220: return "AirPods (1st gen)";
    case 0x0F20: return "AirPods (2nd gen)";
    case 0x1320: return "AirPods (3rd gen)";
    case 0x0E20: return "AirPods Pro";
    case 0x1420: return "AirPods Pro (2nd gen)";
    case 0x0A20: return "AirPods Max";
    case 0x0320: return "Powerbeats3";
    case 0x0520: return "BeatsX";
    case 0x0620: return "Beats Solo3";
    case 0x0B20: return "Powerbeats Pro";
    case 0x0C20: return "Beats Solo Pro";
    case 0x1020: return "Beats Flex";
    case 0x1120: return "Beats Studio Buds";
    default: return nullptr;
  }
}
static const char* msDeviceType(uint8_t t) {      // Microsoft Connected Devices Platform beacon
  switch (t) {
    case 1: return "Xbox";
    case 6: return "iPhone (Microsoft app)";
    case 7: return "iPad (Microsoft app)";
    case 8: return "Android (Microsoft app)";
    case 9: return "Windows desktop PC";
    case 11: return "Windows phone";
    case 12: return "Linux device";
    case 13: return "Windows IoT device";
    case 14: return "Surface Hub";
    case 15: return "Windows laptop";
    default: return nullptr;
  }
}

// Best available human label for a BLE advertiser, shared with the Signal Hunt tool.
// Order: broadcast name, then model details in the manufacturer data, then pairing beacons,
// advertised services and appearance. svcList is space separated; appearance < 0 means none.
String bleDescribe(const char* name, const uint8_t* mfg, uint8_t mfgLen, const char* svcList,
                   const char* svcDataUuid, const uint8_t* svcData, uint8_t svcDataLen, int appearance) {
  if (name && name[0]) return name;
  String company;
  if (mfg && mfgLen >= 2) {
    uint16_t id = mfg[0] | (mfg[1] << 8);
    if (id == 0x004C && mfgLen >= 3) {
      uint8_t t = mfg[2];
      if (t == 0x07 && mfgLen >= 7) {                     // proximity pairing: 07 len 01 model(2) ...
        uint16_t model = (mfg[5] << 8) | mfg[6];
        const char* m = appleAudioModel(model);
        if (m) return m;
        char b[32]; snprintf(b, sizeof(b), "Apple audio (model %04X)", model);
        return b;
      }
      if (t == 0x02) return "iBeacon";
      if (appleType(t)) return String("Apple ") + appleType(t);
    }
    if (id == 0x0006 && mfgLen >= 4) {
      if (mfg[2] == 0x03) {                               // Swift Pair: 03 sub 80 [addr|class] name
        int off = mfg[3] == 0x01 ? 11 : mfg[3] == 0x02 ? 8 : 5;
        if (off < mfgLen) {
          String n;
          for (int i = off; i < mfgLen && n.length() < 24; i++) if (mfg[i] >= 32 && mfg[i] < 127) n += (char)mfg[i];
          if (n.length()) return n + " (Swift Pair)";
        }
        return "Swift Pair accessory";
      }
      if (mfg[2] == 0x01) { const char* m = msDeviceType(mfg[3] & 0x3F); if (m) return m; }
    }
    const char* c = companyName(id);
    if (c) company = c;
  }
  if (svcDataUuid && svcData && svcDataLen >= 3 && String(svcDataUuid).equalsIgnoreCase("0xfe2c")) {
    char b[36]; snprintf(b, sizeof(b), "Fast Pair device %02X%02X%02X", svcData[0], svcData[1], svcData[2]);
    return b;
  }
  if (svcList && svcList[0]) {
    String s = svcList; s.trim();
    while (s.length()) {
      int sp = s.indexOf(' ');
      String u = sp < 0 ? s : s.substring(0, sp);
      s = sp < 0 ? "" : s.substring(sp + 1);
      const char* n = svcName(u);
      if (n && u != "0x1800" && u != "0x1801") return company.length() ? company + " " + n : String(n);
    }
  }
  if (appearance >= 0 && (appearance >> 6) >= 1 && (appearance >> 6) <= 18)
    return (company.length() ? company + " " : String("")) + appearanceName(appearance);
  if (company.length()) return company + " device";
  return "";
}

class ScanCb : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* a) override {
    BleDev n;
    memset(&n, 0, sizeof(n));
    memcpy(n.addr, a->getAddress().getNative(), 6);
    n.addrType = a->getAddress().getType();
    n.rssi = a->getRSSI();
    n.advType = a->getAdvType();
    n.connectable = (n.advType == 0 || n.advType == 1);      // ADV_IND / ADV_DIRECT_IND
    n.flags = a->getAdvFlags();
    if (a->haveTXPower()) { n.hasTx = true; n.txPower = a->getTXPower(); }
    if (a->haveAppearance()) { n.hasAppearance = true; n.appearance = a->getAppearance(); }
    if (a->haveName()) strlcpy(n.name, a->getName().c_str(), sizeof(n.name));
    if (a->haveManufacturerData()) {
      std::string m = a->getManufacturerData();
      n.mfgLen = min(m.size(), sizeof(n.mfg));
      memcpy(n.mfg, m.data(), n.mfgLen);
    }
    String sv;
    for (int i = 0; i < a->getServiceUUIDCount() && sv.length() < 60; i++) { sv += a->getServiceUUID(i).toString().c_str(); sv += ' '; }
    strlcpy(n.svc, sv.c_str(), sizeof(n.svc));
    if (a->haveServiceData()) {
      std::string sd = a->getServiceData(0);
      n.svcDataLen = min(sd.size(), sizeof(n.svcData));
      memcpy(n.svcData, sd.data(), n.svcDataLen);
      strlcpy(n.svcDataUuid, a->getServiceDataUUID(0).toString().c_str(), sizeof(n.svcDataUuid));
    }
    n.rawLen = min((size_t)a->getPayloadLength(), sizeof(n.raw));
    memcpy(n.raw, a->getPayload(), n.rawLen);

    uint32_t now = millis();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_advTotal++;
    if (!s_b) { xSemaphoreGive(s_lock); return; }
    BleDev* d = nullptr;
    for (int i = 0; i < s_nb; i++) if (!memcmp(s_b[i].addr, n.addr, 6) && s_b[i].addrType == n.addrType) { d = &s_b[i]; break; }
    if (!d && s_nb < MAXBLE) {
      d = &s_b[s_nb++];
      *d = n;
      d->first = now; d->rssiMin = d->rssiMax = n.rssi; d->count = 0;
    }
    if (d) {
      if (d->count && now > d->last) {
        uint32_t gap = now - d->last;
        if (gap < 10000) d->intervalMs = d->intervalMs ? (d->intervalMs * 7 + gap) / 8 : gap;
      }
      d->count++; d->last = now; d->rssi = n.rssi;
      if (n.rssi < d->rssiMin) d->rssiMin = n.rssi;
      if (n.rssi > d->rssiMax) d->rssiMax = n.rssi;
      if (n.advType != 4) { d->advType = n.advType; d->connectable = n.connectable; }   // a scan response says nothing about the PDU type
      if (n.flags) d->flags = n.flags;
      if (n.hasTx) { d->hasTx = true; d->txPower = n.txPower; }
      if (n.hasAppearance) { d->hasAppearance = true; d->appearance = n.appearance; }
      if (n.name[0]) memcpy(d->name, n.name, sizeof(n.name));
      if (n.mfgLen) { memcpy(d->mfg, n.mfg, n.mfgLen); d->mfgLen = n.mfgLen; }
      if (n.svc[0]) memcpy(d->svc, n.svc, sizeof(n.svc));
      if (n.svcDataLen) { memcpy(d->svcData, n.svcData, n.svcDataLen); d->svcDataLen = n.svcDataLen; memcpy(d->svcDataUuid, n.svcDataUuid, sizeof(n.svcDataUuid)); }
      if (n.rawLen >= d->rawLen) { memcpy(d->raw, n.raw, n.rawLen); d->rawLen = n.rawLen; }
    }
    xSemaphoreGive(s_lock);
  }
};
static ScanCb s_cb;

static String kindOf(const BleDev& d) {       // short human label for the list
  return bleDescribe(d.name, d.mfg, d.mfgLen, d.svc, d.svcDataUuid, d.svcData, d.svcDataLen, d.hasAppearance ? d.appearance : -1);
}

static void bleDetail(const BleDev& d0) {
  uint8_t key[6]; memcpy(key, d0.addr, 6);
  uint8_t be[6]; for (int i = 0; i < 6; i++) be[i] = d0.addr[5 - i];
  String vendor = d0.addrType == 0 ? wifiVendor(be) : String();
  ui::pager("BLE device detail", [&](std::vector<String>& L) {
    BleDev d = d0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_nb; i++) if (!memcmp(s_b[i].addr, key, 6)) { d = s_b[i]; break; }
    xSemaphoreGive(s_lock);
    char b[80];
    L.push_back("#Identity");
    L.push_back(String("Name\t") + (d.name[0] ? d.name : "<not broadcast>"));
    if (!d.name[0]) { String id = kindOf(d); if (id.length()) L.push_back("Identified as\t" + id); }
    L.push_back("Address\t" + addrStr(d));
    L.push_back(String("Addr type\t") + addrTypeName(d));
    if (vendor.length()) L.push_back("Vendor\t" + vendor);
    if (d.hasAppearance) { snprintf(b, sizeof(b), "Appearance\t0x%04X %s", d.appearance, appearanceName(d.appearance)); L.push_back(b); }
    L.push_back("#Signal");
    snprintf(b, sizeof(b), "RSSI\t%d dBm (min %d max %d)", d.rssi, d.rssiMin, d.rssiMax); L.push_back(b);
    if (d.hasTx) {
      snprintf(b, sizeof(b), "TX power\t%d dBm", d.txPower); L.push_back(b);
      snprintf(b, sizeof(b), "Path loss\t%d dB", d.txPower - d.rssi); L.push_back(b);
    }
    snprintf(b, sizeof(b), "Distance\t~%.1f m (rough)", powf(10.0f, ((d.hasTx ? d.txPower - 41 : -59) - d.rssi) / 20.0f)); L.push_back(b);
    L.push_back("#Advertising");
    L.push_back(String("PDU type\t") + advTypeName(d.advType));
    L.push_back(String("Connectable\t") + (d.connectable ? "yes" : "no"));
    if (d.flags) {
      String f;
      if (d.flags & 0x01) f += "LE-limited ";
      if (d.flags & 0x02) f += "LE-general ";
      if (d.flags & 0x04) f += "no-BR/EDR ";
      if (d.flags & 0x18) f += "dual-mode ";
      snprintf(b, sizeof(b), "Flags\t0x%02X %s", d.flags, f.c_str()); L.push_back(b);
    }
    snprintf(b, sizeof(b), "Packets seen\t%lu", (unsigned long)d.count); L.push_back(b);
    if (d.intervalMs) { snprintf(b, sizeof(b), "Adv interval\t~%lu ms observed", (unsigned long)d.intervalMs); L.push_back(b); }
    snprintf(b, sizeof(b), "First seen\t%lu s ago", (unsigned long)((millis() - d.first) / 1000)); L.push_back(b);
    snprintf(b, sizeof(b), "Last seen\t%lu s ago", (unsigned long)((millis() - d.last) / 1000)); L.push_back(b);
    if (d.mfgLen >= 2) {
      L.push_back("#Manufacturer data");
      uint16_t id = d.mfg[0] | (d.mfg[1] << 8);
      const char* c = companyName(id);
      snprintf(b, sizeof(b), "Company\t0x%04X %s", id, c ? c : "(unlisted)"); L.push_back(b);
      if (id == 0x004C && d.mfgLen >= 3) {
        const char* t = appleType(d.mfg[2]);
        snprintf(b, sizeof(b), "Apple type\t0x%02X %s", d.mfg[2], t ? t : ""); L.push_back(b);
        if (d.mfg[2] == 0x02 && d.mfgLen >= 25) {
          L.push_back("iBeacon UUID\t" + ui::hex(d.mfg + 4, 8));
          L.push_back(" \t" + ui::hex(d.mfg + 12, 8));
          snprintf(b, sizeof(b), "Major / Minor\t%u / %u", (d.mfg[20] << 8) | d.mfg[21], (d.mfg[22] << 8) | d.mfg[23]); L.push_back(b);
          snprintf(b, sizeof(b), "Power @1m\t%d dBm", (int8_t)d.mfg[24]); L.push_back(b);
        }
      }
      if (id == 0x0006 && d.mfgLen >= 3) L.push_back(String("MS beacon\t") + (d.mfg[2] == 0x01 ? "Connected Devices Platform" : d.mfg[2] == 0x03 ? "Swift Pair" : "other"));
      for (int o = 2; o < d.mfgLen; o += 12) L.push_back(String(o == 2 ? "Payload\t" : " \t") + ui::hex(d.mfg + o, min(12, d.mfgLen - o), ' '));
    }
    if (d.svc[0]) {
      L.push_back("#Services advertised");
      String s = d.svc; s.trim();
      while (s.length()) {
        int sp = s.indexOf(' ');
        String u = sp < 0 ? s : s.substring(0, sp);
        s = sp < 0 ? "" : s.substring(sp + 1);
        const char* n = svcName(u);
        if (u.length() > 30) { L.push_back(u.substring(0, 18)); L.push_back(" " + u.substring(18)); }
        else L.push_back(n ? u + "\t" + n : u);
      }
    }
    if (d.svcDataLen) {
      L.push_back("#Service data");
      L.push_back(String("UUID\t") + d.svcDataUuid);
      for (int o = 0; o < d.svcDataLen; o += 12) L.push_back(String(o == 0 ? "Data\t" : " \t") + ui::hex(d.svcData + o, min(12, d.svcDataLen - o), ' '));
    }
    L.push_back("#Raw advertisement");
    for (int o = 0; o < d.rawLen; o += 13) L.push_back(ui::hex(d.raw + o, min(13, d.rawLen - o), ' '));
  }, 1000, "ble_dev");
}

// ------------------------------------------------------------------ connect + read (GATT)
// Opt-in: connects to one device the user picked, lists its services and characteristics, reads
// every readable value, then disconnects. Pairing with a passkey is offered for devices that need it.
static uint32_t s_passkey = 0;
class GattCb : public NimBLEClientCallbacks {
  uint32_t onPassKeyRequest() override { return s_passkey; }
  bool onConfirmPIN(uint32_t) override { return true; }
};
static GattCb s_gattCb;

static const char* gattName(const String& u) {
  struct S { const char* id; const char* n; };
  static const S T[] = {
    {"0x1800", "Generic Access"}, {"0x1801", "Generic Attribute"}, {"0x180a", "Device Information"},
    {"0x180f", "Battery"}, {"0x180d", "Heart Rate"}, {"0x1812", "HID"}, {"0x181a", "Environmental Sensing"},
    {"0x1805", "Current Time"}, {"0x1804", "TX Power"}, {"0x1802", "Immediate Alert"}, {"0x1803", "Link Loss"},
    {"0x2a00", "Device Name"}, {"0x2a01", "Appearance"}, {"0x2a04", "Preferred Conn Params"},
    {"0x2a05", "Service Changed"}, {"0x2a19", "Battery Level"}, {"0x2a23", "System ID"},
    {"0x2a24", "Model Number"}, {"0x2a25", "Serial Number"}, {"0x2a26", "Firmware Revision"},
    {"0x2a27", "Hardware Revision"}, {"0x2a28", "Software Revision"}, {"0x2a29", "Manufacturer Name"},
    {"0x2a50", "PnP ID"}, {"0x2a37", "Heart Rate Measurement"}, {"0x2a38", "Body Sensor Location"},
    {"0x2a6e", "Temperature"}, {"0x2a6f", "Humidity"}, {"0x2a6d", "Pressure"}, {"0x2a07", "TX Power Level"},
    {"0x2a2b", "Current Time"}, {"0x2aa6", "Central Address Resolution"}, {"0x2a4a", "HID Information"},
  };
  for (auto& t : T) if (u.equalsIgnoreCase(t.id)) return t.n;
  return nullptr;
}

static String gattValue(const String& uuid, const std::string& v) {
  char b[48];
  const uint8_t* d = (const uint8_t*)v.data();
  size_t n = v.size();
  if (!n) return "(empty)";
  if (uuid.equalsIgnoreCase("0x2a19") && n >= 1) { snprintf(b, sizeof(b), "%d %%", d[0]); return b; }
  if (uuid.equalsIgnoreCase("0x2a01") && n >= 2) { uint16_t a = d[0] | (d[1] << 8); snprintf(b, sizeof(b), "0x%04X %s", a, appearanceName(a)); return b; }
  if (uuid.equalsIgnoreCase("0x2a07") && n >= 1) { snprintf(b, sizeof(b), "%d dBm", (int8_t)d[0]); return b; }
  if (uuid.equalsIgnoreCase("0x2a6e") && n >= 2) { snprintf(b, sizeof(b), "%.2f C", (int16_t)(d[0] | (d[1] << 8)) / 100.0f); return b; }
  if (uuid.equalsIgnoreCase("0x2a6f") && n >= 2) { snprintf(b, sizeof(b), "%.2f %%", (uint16_t)(d[0] | (d[1] << 8)) / 100.0f); return b; }
  if (uuid.equalsIgnoreCase("0x2a50") && n >= 7) {
    snprintf(b, sizeof(b), "vendor %04X product %04X ver %04X", d[1] | (d[2] << 8), d[3] | (d[4] << 8), d[5] | (d[6] << 8));
    return b;
  }
  bool text = true;
  for (size_t i = 0; i < n; i++) if (d[i] < 32 || d[i] > 126) { text = false; break; }
  if (text) return String("\"") + String(v.c_str()).substring(0, 60) + "\"";
  return ui::hex(d, min(n, (size_t)20), ' ') + (n > 20 ? " ..." : "");
}

static void gattExplore(const BleDev& d, NimBLEScan* scan) {
  static const char* const how[] = {"Connect", "Pair with passkey, then connect"};
  int mode = ui::popup("Connect to device", how, 2);
  if (mode < 0) return;
  if (mode == 1) {
    String pk = "000000";
    if (!ui::input("Passkey (6 digits)", pk, 6)) return;
    s_passkey = pk.toInt();
    NimBLEDevice::setSecurityAuth(false, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  } else {
    NimBLEDevice::setSecurityAuth(false, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  }
  auto status = [&](const String& msg) {
    ui::clear(); ui::header("BLE connect");
    cv.setTextColor(C_FG, C_BG); cv.setCursor(4, 44); cv.printf("%.38s", kindOf(d).length() ? kindOf(d).c_str() : addrStr(d).c_str());
    cv.setTextColor(C_DIM, C_BG); cv.setCursor(4, 58); cv.print(msg);
    ui::footer("please wait"); ui::push();
  };
  scan->stop();
  status("connecting (up to 8 s)...");
  uint8_t be[6];
  for (int i = 0; i < 6; i++) be[i] = d.addr[5 - i];
  NimBLEClient* c = NimBLEDevice::createClient();
  c->setClientCallbacks(&s_gattCb, false);
  c->setConnectTimeout(8);
  std::vector<String> lines;
  bool ok = c->connect(NimBLEAddress(be, d.addrType));
  if (!ok) {
    NimBLEDevice::deleteClient(c);
    ui::toast("Could not connect (out of range or refused)", C_BAD, 1800);
    return;
  }
  char b[80];
  bool paired = false;
  if (mode == 1) { status("pairing..."); paired = c->secureConnection(); }
  lines.push_back("#Connection");
  lines.push_back("Address\t" + addrStr(d));
  snprintf(b, sizeof(b), "Signal\t%d dBm", c->getRssi()); lines.push_back(b);
  snprintf(b, sizeof(b), "MTU\t%d bytes", c->getMTU()); lines.push_back(b);
  if (mode == 1) lines.push_back(String("Pairing\t") + (paired ? "succeeded, link encrypted" : "failed (wrong passkey?)"));
  status("reading services...");
  int nSvc = 0, nChr = 0, nRead = 0, nDenied = 0;
  std::vector<NimBLERemoteService*>* svcs = c->getServices(true);
  if (svcs) {
    for (auto* sv : *svcs) {
      if (!c->isConnected() || lines.size() > 400) break;
      String su = sv->getUUID().toString().c_str();
      const char* sn = gattName(su);
      lines.push_back("#" + (sn ? String(sn) : su.substring(0, 36)));
      if (sn) lines.push_back("Service UUID\t" + su);
      nSvc++;
      std::vector<NimBLERemoteCharacteristic*>* chs = sv->getCharacteristics(true);
      if (!chs) continue;
      for (auto* ch : *chs) {
        if (!c->isConnected()) break;
        String cu = ch->getUUID().toString().c_str();
        const char* cn = gattName(cu);
        String props;
        if (ch->canRead()) props += "read ";
        if (ch->canWrite() || ch->canWriteNoResponse()) props += "write ";
        if (ch->canNotify()) props += "notify ";
        if (ch->canIndicate()) props += "indicate ";
        nChr++;
        lines.push_back(cn ? String(cn) : (cu.length() > 12 ? cu.substring(0, 23) + "~" : cu));
        lines.push_back(" \t" + props);
        if (ch->canRead()) {
          snprintf(b, sizeof(b), "reading %d values...", nRead + 1); status(b);
          std::string v = ch->readValue();
          if (v.size()) { lines.push_back(" \t" + gattValue(cu, v)); nRead++; }
          else { lines.push_back(" \t(no value / access denied)"); nDenied++; }
        }
        M5Cardputer.update();
      }
    }
  }
  bool dropped = !c->isConnected();
  if (!dropped) c->disconnect();
  NimBLEDevice::deleteClient(c);
  snprintf(b, sizeof(b), "Found\t%d services, %d characteristics", nSvc, nChr); lines.insert(lines.begin() + 1, b);
  snprintf(b, sizeof(b), "Values read\t%d (%d denied)", nRead, nDenied); lines.insert(lines.begin() + 2, b);
  if (dropped) lines.insert(lines.begin() + 3, "Note\tthe device closed the connection early");
  if (nDenied && mode == 0) lines.push_back("Some values need pairing: connect again and choose the passkey option.");
  ui::pager("BLE device contents", [&](std::vector<String>& L) { L = lines; }, 0, "ble_gatt");
}

void appBle() {
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  s_b = (BleDev*)calloc(MAXBLE, sizeof(BleDev));
  if (!s_b) { ui::toast("Out of memory", C_BAD); return; }
  s_nb = 0; s_advTotal = 0;
  ui::clear(); ui::header("BLE Scan"); cv.setTextColor(C_DIM, C_BG); cv.setCursor(60, 60); cv.print("Starting Bluetooth..."); ui::push();
  NimBLEDevice::init("");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&s_cb, true);
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(80);
  scan->setMaxResults(0);            // do not let the library keep its own copy
  scan->start(0, nullptr, false);
  bool active = true, running = true;

  int sel = 0, top = 0, sortMode = 0;
  uint32_t lastDraw = 0, rateAt = millis(), lastTotal = 0, rate = 0;
  std::vector<BleDev> snap;

  while (true) {
    ui::Key k = ui::poll();
    if (k.back) break;
    if (k.up && sel > 0) sel--;
    if (k.down && sel + 1 < (int)snap.size()) sel++;
    if (k.enter && sel < (int)snap.size()) bleDetail(snap[sel]);
    if (k.lc == 'g' && sel < (int)snap.size()) {        // connect and read the device
      gattExplore(snap[sel], scan);
      if (running) scan->start(0, nullptr, false);
    }
    if (k.lc == 'o') sortMode = (sortMode + 1) % 3;
    if (k.lc == 'p') {                       // pause / resume
      running = !running;
      if (running) scan->start(0, nullptr, false); else scan->stop();
    }
    if (k.lc == 'a') {                       // active <-> passive scan
      active = !active;
      scan->stop();
      scan->setActiveScan(active);
      if (running) scan->start(0, nullptr, false);
    }
    if (k.lc == 'x') { xSemaphoreTake(s_lock, portMAX_DELAY); s_nb = 0; xSemaphoreGive(s_lock); sel = 0; }
    if (k.lc == 's' && !snap.empty()) {
      if (!ui::sdMount()) ui::toast("No SD card", C_BAD);
      else {
        String p = ui::nextFile("ble", "csv");
        File f = SD.open(p, FILE_WRITE);
        if (f) {
          f.println("address,addr_type,name,rssi,tx_power,connectable,company_id,company,services,packets,interval_ms,mfg_data,raw");
          for (auto& d : snap) {
            uint16_t id = d.mfgLen >= 2 ? (d.mfg[0] | (d.mfg[1] << 8)) : 0;
            const char* c = d.mfgLen >= 2 ? companyName(id) : nullptr;
            f.printf("%s,%s,\"%s\",%d,%s,%d,", addrStr(d).c_str(), d.addrType == 0 ? "public" : "random", d.name, d.rssi,
                     d.hasTx ? String(d.txPower).c_str() : "", d.connectable);
            if (d.mfgLen >= 2) f.printf("0x%04X", id);
            f.printf(",%s,%s,%lu,%lu,%s,%s\n", c ? c : "", d.svc, (unsigned long)d.count, (unsigned long)d.intervalMs,
                     ui::hex(d.mfg, d.mfgLen).c_str(), ui::hex(d.raw, d.rawLen).c_str());
          }
          f.close();
          ui::toast("Saved " + p.substring(11));
        } else ui::toast("SD write failed", C_BAD);
      }
    }
    if (millis() - rateAt >= 1000) { rate = s_advTotal - lastTotal; lastTotal = s_advTotal; rateAt = millis(); }
    if (millis() - lastDraw < 150) { delay(5); continue; }
    lastDraw = millis();

    String keep = sel < (int)snap.size() ? addrStr(snap[sel]) : String();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snap.assign(s_b, s_b + s_nb);
    xSemaphoreGive(s_lock);
    std::sort(snap.begin(), snap.end(), [&](const BleDev& a, const BleDev& b) {
      if (sortMode == 1) return a.last > b.last;
      if (sortMode == 2) return (a.name[0] != 0) != (b.name[0] != 0) ? a.name[0] != 0 : strcmp(a.name, b.name) < 0;
      return a.rssi > b.rssi;
    });
    for (int i = 0; i < (int)snap.size(); i++) if (addrStr(snap[i]) == keep) sel = i;
    if (sel >= (int)snap.size()) sel = max(0, (int)snap.size() - 1);

    ui::clear();
    char right[28];
    snprintf(right, sizeof(right), "%d dev %lu/s %s", (int)snap.size(), (unsigned long)rate, !running ? "PAUSE" : active ? "act" : "pas");
    ui::header("BLE Scan", right);
    if (sel < top) top = sel;
    if (sel >= top + ROWS) top = sel - ROWS + 1;
    for (int r = 0; r < ROWS && top + r < (int)snap.size(); r++) {
      auto& d = snap[top + r];
      int y = TOP + r * LINE_H;
      uint16_t bg = (top + r == sel) ? C_SEL : C_BG;
      if (top + r == sel) cv.fillRect(0, y - 1, SCR_W, LINE_H, bg);
      String label = kindOf(d);
      char nm[24];
      if (label.length()) snprintf(nm, sizeof(nm), "%-22.22s", label.c_str());
      else snprintf(nm, sizeof(nm), "%s", addrStr(d).c_str());
      bool stale = millis() - d.last > 10000;
      cv.setTextColor(stale ? C_DIM : d.name[0] ? C_FG : label.length() ? C_ACC : C_DIM, bg); cv.setCursor(2, y); cv.print(nm);
      cv.setTextColor(d.connectable ? C_OK : C_DIM, bg); cv.setCursor(140, y); cv.print(d.connectable ? "C" : "-");
      cv.setTextColor(d.addrType == 0 ? C_YEL : C_DIM, bg); cv.setCursor(150, y); cv.print(d.addrType == 0 ? "P" : "R");
      cv.setTextColor(stale ? C_DIM : d.rssi > -60 ? C_OK : d.rssi > -80 ? C_YEL : C_WARN, bg); cv.setCursor(164, y); cv.printf("%4d", d.rssi);
      int q = constrain((d.rssi + 100) * 40 / 70, 0, 40);
      cv.drawRect(194, y, 42, 7, C_SEL);
      cv.fillRect(195, y + 1, q, 5, stale ? C_DIM : d.rssi > -60 ? C_OK : d.rssi > -80 ? C_YEL : C_WARN);
    }
    if (snap.empty()) { cv.setTextColor(C_DIM, C_BG); cv.setCursor(76, 60); cv.print("Scanning..."); }
    ui::footer(sortMode == 0 ? "ENTER detail G connect S O:rssi P A X" : sortMode == 1 ? "ENTER detail G connect S O:new P A X" : "ENTER detail G connect S O:name P A X");
    ui::push();
  }
  scan->stop();
  NimBLEDevice::deinit(true);
  xSemaphoreTake(s_lock, portMAX_DELAY);
  free(s_b); s_b = nullptr; s_nb = 0;
  xSemaphoreGive(s_lock);
}
