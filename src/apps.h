#pragma once
void appHunt();
void appMic();
void appPlayer();
void appImu();
void appWifiScan();
void appWifiMonitor();
void appBle();
void appBattery();
void appPins();
void appChip();
void appI2c();
void appSd();                                             // File Manager
void appEditor();                                         // Text Editor (stand-alone entry)

// Cross-tool entry points
#include <Arduino.h>
int playerPlay(const String& path, const String& name);  // +1 finished, 0 stopped, -1 error
void editorOpen(const String& path);                     // edit an existing text file
String filesPick(const char* title);                     // file chooser; "" when cancelled
bool filesIsText(const String& name);
// Join a WiFi network the user has the password for and inspect it from the inside (app_net.cpp)
void wifiJoin(const uint8_t* bssid, const char* ssid, int channel, int authmode);
void appKeys();
void appIr();
void appScreen();
void appSettings();
void appWeb();                                            // Web mode, started from Settings
bool webActive();                                         // web mode running (possibly in the background)
void webService();                                        // serve pending browser requests; called from ui::poll()
void webStop();
String gpioJson();                                        // live pin table for the web page (app_sys.cpp)
