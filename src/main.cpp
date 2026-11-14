// MUHAX Logger - NFC check-in / check-out terminal for the ESP32-2432S028R "CYD".
// See README.md for setup and the muhax-logger.md plan for the design.
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "config.h"
#include "app.h"
#include "settings.h"
#include "storage.h"
#include "clock.h"
#include "ui.h"
#include "hw.h"
#include "web.h"
#include "github.h"

enum Mode { M_FIRST_SETUP, M_RUN, M_RECOVERY, M_FORCED_SETUP };
enum Screen { SC_IDLE, SC_SETUP, SC_RESULT, SC_INFO };

static Mode mode = M_RUN;
static Screen screen = SC_IDLE;
static uint32_t screenUntil = 0;          // result / info screens return to base after this
String g_apPass;
int g_tapsToday = 0;

static uint32_t restartAt = 0;
static bool wantForcedSetup = false;
static uint32_t lastConnectedMs = 0, connectedSince = 0;
static bool wifiWas = false, mdnsStarted = false, startupInfoShown = false;
static uint32_t ledOffAt = 0;
static uint32_t lastActivity = 0;
static uint32_t lastIdleTick = 0;
static uint32_t lastTouchMs = 0;
static String lastTapUid;
static uint32_t lastTapSeenMs = 0, lastDupShown = 0;
static String lastText;                   // "Ahmad  IN 09:58"
static String tapsDay;

void app_requestRestart(uint32_t delayMs) { restartAt = millis() + delayMs; }
void app_enterSetupMode() { wantForcedSetup = true; }

// ------------------------------------------------------------------ helpers
static String fmtDuration(uint32_t s) {
  uint32_t h = s / 3600, m = (s % 3600) / 60;
  return h ? String(h) + "h " + String(m) + "m" : String(m) + "m";
}

static String ago(uint32_t s) {
  if (s == 0xFFFFFFFFUL) return "never";
  if (s < 90) return String(s) + " s ago";
  if (s < 5400) return String(s / 60) + " min ago";
  return String(s / 3600) + " h ago";
}

static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
static bool inSetupMode() { return mode != M_RUN; }

static void showBase() {
  if (inSetupMode()) {
    Ui::setupScreen(SETUP_SSID, g_apPass, SETUP_IP_STR);
    screen = SC_SETUP;
  } else {
    Ui::idleFull();
    screen = SC_IDLE;
  }
  screenUntil = 0;
}

static void wake() {
  lastActivity = millis();
  Ui::setBacklight(255);
}

static void showResultFor(uint32_t ms) {
  screen = SC_RESULT;
  screenUntil = millis() + ms;
}

static void showInfo(uint32_t ms) {
  InfoData d;
  d.company = cfg.company;
  if (wifiUp()) d.wifi = cfg.wifiSsid + " (connected)";
  else d.wifi = inSetupMode() ? "setup network " SETUP_SSID : (cfg.wifiSsid + " (offline)");
  d.adminIp = wifiUp() ? "http://" + WiFi.localIP().toString() : "(no network)";
  d.adminMdns = wifiUp() ? "http://" HOSTNAME ".local" : "";
  d.time = Clock::synced() ? "synced " + ago(Clock::secondsSinceSync()) + " (every " + String(cfg.syncHours) + " h)" : "not synced yet";
  if (Storage::ok()) d.sd = "OK (" + String(Storage::listDays().size()) + " days)";
  else d.sd = "MISSING";
  int q = Storage::queuedCount(), p = Storage::pendingCount();
  if (q) d.extra = String(q) + " taps waiting in flash";
  else if (p) d.extra = String(p) + " taps waiting for time";
  int gs = Github::state();
  d.github = gs == 0 ? "not configured" : (gs == 1 ? "synced " + ago(Github::secondsSinceOk()) : (Github::lastError().length() ? "error: " + Github::lastError() : "waiting"));
  Ui::info(d);
  screen = SC_INFO;
  screenUntil = millis() + ms;
}

static void startSetupNetwork(Mode m) {
  mode = m;
  g_apPass = randomSetupPassword();
  Web::startAp(g_apPass);
  showBase();
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.wifiSsid.c_str(), cfg.wifiPass.c_str());
  Web::startServer();
}

static int countTapsToday() {
  if (!Clock::synced() || !Storage::ok()) return 0;
  String path = Storage::logPath(Clock::day(Clock::now()));
  size_t off = 0;
  String chunk;
  int n = 0;
  while (true) {
    size_t nx = Storage::readChunk(path, off, chunk, 2048);
    if (nx == off) break;
    off = nx;
    for (size_t i = 0; i < chunk.length(); i++) if (chunk[i] == '\n') n++;
  }
  return n > 0 ? n - 1 : 0;       // minus the header line
}

static void onTimeSynced() {
  Storage::resolvePending();
  Storage::loadTodayTotals();
  tapsDay = Clock::day(Clock::now());
  g_tapsToday = countTapsToday();
}

// ------------------------------------------------------------------ tap handling
static void handleTap(const String& uid) {
  wake();
  uint32_t nowMs = millis();

  if (!cfg.configured) {                       // first-time setup: just echo the card id
    Ui::warning("Card detected", uid, "Use 'Tap card now' in the setup page");
    Led::set(false, false, true);
    ledOffAt = nowMs + RESULT_SCREEN_MS;
    showResultFor(RESULT_SCREEN_MS);
    return;
  }

  // Same card still on / just back on the reader: ignore (the window restarts while it stays there).
  if (uid == lastTapUid && nowMs - lastTapSeenMs < (uint32_t)cfg.dupSeconds * 1000UL) {
    lastTapSeenMs = nowMs;
    if (nowMs - lastDupShown > 4000UL) {
      lastDupShown = nowMs;
      Ui::warning("Already tapped", "", "Counted once");
      Led::set(true, true, false);
      ledOffAt = nowMs + 1200;
      showResultFor(1200);
    }
    return;
  }
  lastTapUid = uid;
  lastTapSeenMs = nowMs;

  Worker* w = Storage::byUid(uid);
  if (!w) {
    Storage::addUnknown(uid);
    Ui::warning("Unknown card", uid, "Ask the admin to register it");
    Led::set(true, true, false);
    ledOffAt = nowMs + RESULT_SCREEN_MS;
    showResultFor(RESULT_SCREEN_MS);
    return;
  }
  if (!w->active) {
    Ui::warning("Card disabled", w->name, "Contact the admin");
    Led::set(true, true, false);
    ledOffAt = nowMs + RESULT_SCREEN_MS;
    showResultFor(RESULT_SCREEN_MS);
    return;
  }

  bool timeOk = Clock::synced();
  time_t now = Clock::now();
  bool nextIn = true;
  String flag;
  if (w->in) {
    if (!timeOk || w->lastEpoch == 0 || Clock::day(w->lastEpoch) == Clock::day(now)) nextIn = false;
    else flag = "MISSING_OUT_PREV";            // forgot to tap out on an earlier day
  }

  Record r;
  r.uid = w->uid;
  r.empId = w->id;
  r.name = w->name;
  r.in = nextIn;
  r.flag = flag;
  if (timeOk) { r.epoch = now; r.trusted = Clock::trusted(); }
  else { r.pending = true; r.trusted = false; }

  if (!Storage::commit(r)) {
    Ui::warning("NOT SAVED", "Storage full / SD missing", "Tell the admin");
    Led::set(true, false, false);
    ledOffAt = nowMs + 4000;
    showResultFor(4000);
    return;
  }

  if (timeOk) {
    String d = Clock::day(now);
    if (d != tapsDay) { tapsDay = d; g_tapsToday = 0; }
  }
  g_tapsToday++;

  String hhmm = timeOk ? Clock::hhmm(now) : "--:--";
  String worked = (!nextIn && w->workedToday > 0) ? fmtDuration(w->workedToday) : "";
  Ui::welcome(nextIn, w->name, w->id, hhmm, timeOk ? Clock::longDate(now) : "", worked);
  Led::set(!nextIn, nextIn, false);            // green = IN, red = OUT
  ledOffAt = nowMs + RESULT_SCREEN_MS;
  lastText = w->name + (nextIn ? "  IN " : "  OUT ") + hhmm;
  showResultFor(RESULT_SCREEN_MS);
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);   // UART0 is free: the PN532 uses GPIO35/22 (Serial2)
  pinMode(PIN_BOOT, INPUT_PULLUP);
  Led::begin();
  Ui::begin();
  Ui::splash("Starting...");
  cfg.load();
  Touch::begin();
  Storage::begin();
  Nfc::begin();
  Web::begin();
  Github::begin();
  Serial.printf("MUHAX Logger %s (%s) SD=%d NFC=%d configured=%d\n", FW_VERSION, cfg.deviceId.c_str(), Storage::ok(), Nfc::ready(), cfg.configured);

  lastActivity = millis();
  if (!cfg.configured) {
    startSetupNetwork(M_FIRST_SETUP);
    return;
  }

  mode = M_RUN;
  Ui::splash("Connecting WiFi", cfg.wifiSsid);
  connectWifi();
  uint32_t t0 = millis();
  while (!wifiUp() && millis() - t0 < 15000) { delay(100); Web::loop(); }
  lastConnectedMs = millis();
  if (!Storage::ok()) {
    Ui::warning("SD card missing", "Insert a FAT32 card", "Taps are kept in memory");
    delay(2500);
  }
  showBase();
}

void loop() {
  uint32_t nowMs = millis();
  Web::loop();
  Storage::loop();
  Clock::loop();

  if (restartAt && (int32_t)(nowMs - restartAt) >= 0) { delay(100); ESP.restart(); }

  // ---- WiFi events
  bool up = wifiUp();
  if (up && !wifiWas) {
    connectedSince = nowMs;
    Clock::begin();
    if (!mdnsStarted && MDNS.begin(HOSTNAME)) { MDNS.addService("http", "tcp", 80); mdnsStarted = true; }
    if (!startupInfoShown && mode == M_RUN) { startupInfoShown = true; showInfo(STARTUP_INFO_MS); }
  }
  wifiWas = up;
  if (up) lastConnectedMs = nowMs;

  // ---- setup / recovery transitions
  if (wantForcedSetup && mode != M_FORCED_SETUP) {
    wantForcedSetup = false;
    startSetupNetwork(M_FORCED_SETUP);
  }
  if (mode == M_RUN && cfg.configured && !up && nowMs - lastConnectedMs > WIFI_LOST_RECOVERY_MS)
    startSetupNetwork(M_RECOVERY);                                  // saved WiFi unusable: setup network returns
  if (mode == M_RECOVERY && up && nowMs - connectedSince > 10000UL) {
    Web::stopAp();                                                  // WiFi is back: leave recovery
    mode = M_RUN;
    showBase();
  }
  if (mode != M_FORCED_SETUP && Boot::heldFor(BOOT_HOLD_SETUP_MS)) wantForcedSetup = true;

  // ---- time sync event
  if (Clock::consumeSyncEvent()) onTimeSynced();

  // ---- NFC
  Nfc::retryInit();
  String uid;
  if (Nfc::poll(uid)) handleTap(uid);

  // ---- touch: info screen
  if (nowMs - lastTouchMs > 500 && Touch::pressed()) {
    lastTouchMs = nowMs;
    wake();
    if (screen == SC_INFO) showBase();
    else if (screen == SC_IDLE) showInfo(15000);
  }

  // ---- LED + screen timers
  if (ledOffAt && (int32_t)(nowMs - ledOffAt) >= 0) { Led::off(); ledOffAt = 0; }
  if ((screen == SC_RESULT || screen == SC_INFO) && (int32_t)(nowMs - screenUntil) >= 0) showBase();

  // ---- idle screen refresh
  if (screen == SC_IDLE && nowMs - lastIdleTick >= 250) {
    lastIdleTick = nowMs;
    IdleData d;
    d.timeOk = Clock::synced();
    if (d.timeOk) { time_t t = Clock::now(); d.clock = Clock::hhmmss(t); d.date = Clock::longDate(t); }
    d.wifi = up;
    d.sd = Storage::ok();
    d.cloud = Github::state();
    d.inNow = Storage::insideCount();
    d.last = Nfc::ready() ? lastText : "NFC reader offline!";
    Ui::idleTick(d);
  }

  // ---- screen dimming (wakes on tap or touch)
  if (cfg.dim && nowMs - lastActivity > 60000UL) Ui::setBacklight(40);
  else Ui::setBacklight(255);

  delay(1);
}
