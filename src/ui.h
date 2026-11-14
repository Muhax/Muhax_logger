#pragma once
#include <Arduino.h>
#include <time.h>

struct IdleData {
  bool timeOk = false;
  String clock, date;
  bool wifi = false, sd = false;
  int cloud = 0;               // 0 off/not configured, 1 ok, 2 pending/error
  int inNow = 0;
  String last;                 // "Ahmad  IN 09:58"
};

struct InfoData {
  String company, wifi, adminIp, adminMdns, time, sd, github, extra;
};

namespace Ui {
void begin();
void setBacklight(uint8_t level);               // 0..255
void splash(const String& title, const String& sub = "");
void setupScreen(const String& ssid, const String& pass, const String& ip);
void idleFull();                                // draw static frame and reset caches
void idleTick(const IdleData& d);               // only redraws what changed
void welcome(bool in, const String& name, const String& id, const String& hhmm, const String& date, const String& worked);
void warning(const String& title, const String& l1, const String& l2 = "", const String& l3 = "");
void info(const InfoData& d);
String ascii(const String& s);                  // Latin-only text for the TFT font
}
