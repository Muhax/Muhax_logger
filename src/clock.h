#pragma once
#include <Arduino.h>
#include <time.h>

// Time handling. The CYD has no battery clock, so time comes from NTP at every start
// and every cfg.syncHours hours (or from the admin's phone as a fallback).
namespace Clock {
void begin();                       // call once WiFi STA is up (safe to call again)
void loop();                        // retries / overdue checks
bool synced();                      // have we ever had a valid time since boot?
bool trusted();                     // synced and last sync younger than 24 h
time_t now();                       // UTC epoch (0 if not synced)
uint32_t secondsSinceSync();
bool consumeSyncEvent();            // true once after each successful sync
void setFromPhone(time_t utc);
void requestSync();                 // "Sync time now"
uint32_t bootSeconds();             // seconds since boot (for pending taps)

void localTm(time_t utc, tm& out);  // applies cfg.tzMinutes
String fmt(time_t utc, const char* f);     // local time, strftime format
String day(time_t utc);                    // "2026-11-12"
String hhmm(time_t utc);
String hhmmss(time_t utc);
String longDate(time_t utc);               // "Thursday, 12 Nov 2026"
bool isValidDay(const String& d);          // "YYYY-MM-DD"
}
