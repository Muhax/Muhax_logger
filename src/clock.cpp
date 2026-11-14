#include "clock.h"
#include "settings.h"
#include "config.h"
#include <WiFi.h>
#include <sys/time.h>
#include <esp_sntp.h>

namespace Clock {

static volatile bool gSynced = false;
static volatile bool gEvent = false;
static volatile uint32_t gLastSyncMs = 0;
static uint32_t gLastKick = 0;
static bool gStarted = false;

static void onSync(struct timeval*) {
  gSynced = true;
  gLastSyncMs = millis();
  gEvent = true;
}

static void kick() {
  sntp_set_time_sync_notification_cb(onSync);
  sntp_set_sync_interval((uint32_t)cfg.syncHours * 3600UL * 1000UL);
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  gLastKick = millis();
  gStarted = true;
}

void begin() { kick(); }

void requestSync() { if (WiFi.status() == WL_CONNECTED) kick(); }

void loop() {
  if (WiFi.status() != WL_CONNECTED) return;
  uint32_t since = millis() - gLastKick;
  if (!gSynced) {
    if (!gStarted || since > 30000UL) kick();            // retry every 30 s until it works
  } else {
    uint32_t overdue = (uint32_t)cfg.syncHours * 3600000UL + 600000UL;
    if ((millis() - gLastSyncMs) > overdue && since > 300000UL) kick();  // re-try every 5 min
  }
}

bool synced() { return gSynced; }

uint32_t secondsSinceSync() { return gSynced ? (millis() - gLastSyncMs) / 1000UL : 0xFFFFFFFFUL; }

bool trusted() { return gSynced && secondsSinceSync() < TIME_UNTRUSTED_AFTER_S; }

time_t now() { return gSynced ? time(nullptr) : 0; }

bool consumeSyncEvent() {
  if (gEvent) { gEvent = false; return true; }
  return false;
}

void setFromPhone(time_t utc) {
  struct timeval tv = {utc, 0};
  settimeofday(&tv, nullptr);
  gSynced = true;
  gLastSyncMs = millis();
  gEvent = true;
}

uint32_t bootSeconds() { return millis() / 1000UL; }

void localTm(time_t utc, tm& out) {
  time_t l = utc + (time_t)cfg.tzMinutes * 60;
  gmtime_r(&l, &out);
}

String fmt(time_t utc, const char* f) {
  tm t;
  localTm(utc, t);
  char b[48];
  strftime(b, sizeof(b), f, &t);
  return String(b);
}

String day(time_t utc) { return fmt(utc, "%Y-%m-%d"); }
String hhmm(time_t utc) { return fmt(utc, "%H:%M"); }
String hhmmss(time_t utc) { return fmt(utc, "%H:%M:%S"); }
String longDate(time_t utc) { return fmt(utc, "%A, %d %b %Y"); }

bool isValidDay(const String& d) {
  if (d.length() != 10 || d[4] != '-' || d[7] != '-') return false;
  for (int i = 0; i < 10; i++) {
    if (i == 4 || i == 7) continue;
    if (d[i] < '0' || d[i] > '9') return false;
  }
  return true;
}

}  // namespace Clock
