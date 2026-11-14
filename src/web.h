#pragma once
#include <Arduino.h>

namespace Web {
void begin();                        // registers routes (does NOT start listening)
void startServer();                  // start listening: call after WiFi is started
void loop();                         // handle clients + captive DNS
void startAp(const String& password);
void stopAp();
bool apActive();
String adminUrl();                   // http://<ip>
}
