#pragma once
#include <Arduino.h>

namespace Github {
void begin();                         // starts the background upload task
void kick();                          // "sync now"
bool test(const String& owner, const String& repo, const String& branch, const String& token, String& message);

// status for the screens / dashboard
int state();                          // 0 not configured, 1 up to date, 2 pending / failing
uint32_t secondsSinceOk();            // 0xFFFFFFFF = never
String lastError();
int pendingDays();
}
