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
void appKeys();
void appIr();
void appScreen();
