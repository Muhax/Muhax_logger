#include "web.h"
#include "app.h"
#include "config.h"
#include "settings.h"
#include "storage.h"
#include "clock.h"
#include "github.h"
#include "hw.h"
#include "page_setup.h"
#include "page_admin.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>

static WebServer server(80);
static DNSServer dns;
static bool gAp = false;

namespace Web {

void startServer();
bool apActive() { return gAp; }

String adminUrl() { return "http://" + WiFi.localIP().toString(); }

void startAp(const String& password) {
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(SETUP_IP), IPAddress(SETUP_IP), IPAddress(255, 255, 255, 0));
  WiFi.softAP(SETUP_SSID, password.c_str(), 1, 0, 4);
  dns.start(53, "*", WiFi.softAPIP());
  gAp = true;
  startServer();
}

void stopAp() {
  if (!gAp) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  gAp = false;
}

// ------------------------------------------------------------------ helpers
static bool fromAp() { return gAp && server.client().localIP() == WiFi.softAPIP(); }

static void sendJson(JsonDocument& d, int code = 200) {
  String s;
  serializeJson(d, s);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", s);
}
static void sendErr(const String& m, int code = 400) {
  JsonDocument d;
  d["error"] = m;
  sendJson(d, code);
}
static void sendOk() {
  JsonDocument d;
  d["ok"] = true;
  sendJson(d);
}
static bool readBody(JsonDocument& d) {
  String b = server.arg("plain");
  if (b.length() == 0) return true;      // empty body = empty object
  return !deserializeJson(d, b);
}

static bool basicAuthOk() {
  if (!cfg.configured) return false;
  String h = server.header("Authorization");
  if (!h.startsWith("Basic ")) return false;
  String b64 = h.substring(6);
  uint8_t out[160];
  size_t olen = 0;
  if (mbedtls_base64_decode(out, sizeof(out) - 1, &olen, (const uint8_t*)b64.c_str(), b64.length()) != 0) return false;
  out[olen] = 0;
  String cred((char*)out);
  int c = cred.indexOf(':');
  if (c < 0) return false;
  return cfg.checkAdmin(cred.substring(0, c), cred.substring(c + 1));
}

static bool needAdmin() {
  if (fromAp()) { sendErr("Use the setup page on the setup network", 403); return false; }
  if (!basicAuthOk()) {
    delay(800);        // slows down password guessing
    server.requestAuthentication(BASIC_AUTH, "MUHAX Logger", "Login required");
    return false;
  }
  return true;
}
static bool needSetup() {
  if (!fromAp()) { sendErr("Setup is only available on the Muhax_logger network", 403); return false; }
  return true;
}
static bool needAny() { return fromAp() || needAdmin(); }

using Fn = void (*)();
static void adminRoute(const char* path, HTTPMethod m, Fn f) {
  server.on(path, m, [f]() { if (needAdmin()) f(); });
}
static void setupRoute(const char* path, HTTPMethod m, Fn f) {
  server.on(path, m, [f]() { if (needSetup()) f(); });
}

static String normUid(String u) {
  String o;
  for (size_t i = 0; i < u.length(); i++) {
    char c = u[i];
    if (c == ' ' || c == ':' || c == '-') continue;
    o += (char)toupper(c);
  }
  return o;
}

// ------------------------------------------------------------------ settings JSON
static void settingsToJson(JsonDocument& d, bool withWorkers) {
  d["configured"] = cfg.configured;
  d["deviceId"] = cfg.deviceId;
  d["admin"]["user"] = cfg.adminUser;
  d["company"] = cfg.company;
  d["wifi"]["ssid"] = cfg.wifiSsid;
  d["tz"] = cfg.tzMinutes;
  d["syncHours"] = cfg.syncHours;
  d["gh"]["owner"] = cfg.ghOwner;
  d["gh"]["repo"] = cfg.ghRepo;
  d["gh"]["branch"] = cfg.ghBranch;
  d["gh"]["tokenSet"] = cfg.ghToken.length() > 0;     // the token itself is never sent back
  d["opts"]["dup"] = cfg.dupSeconds;
  d["opts"]["led"] = cfg.led;
  d["opts"]["dim"] = cfg.dim;
  d["opts"]["uploadDelay"] = cfg.uploadDelaySec;
  if (withWorkers) {
    JsonArray a = d["workers"].to<JsonArray>();
    for (auto& w : Storage::workers) {
      JsonObject o = a.add<JsonObject>();
      o["id"] = w.id; o["name"] = w.name; o["uid"] = w.uid;
    }
  }
}

// Applies a settings document. `setup` also handles admin account + workers. Returns false with err set.
static bool applySettings(JsonVariant d, bool setup, String& err, bool& restart) {
  Settings n = cfg;
  restart = false;

  if (setup) {
    String user = d["admin"]["user"] | "";
    String pass = d["admin"]["pass"] | "";
    user.trim();
    if (!cfg.configured || pass.length()) {
      if (user.length() < 3) { err = "Admin username needs 3+ characters"; return false; }
      if (pass.length() < 8) { err = "Admin password needs 8+ characters"; return false; }
      n.adminUser = user;
      n.setAdminPassword(pass);
    } else if (user.length() >= 3) {
      n.adminUser = user;
    }
  }
  if (!d["company"].isNull()) n.company = String((const char*)(d["company"] | ""));
  if (!d["wifi"]["ssid"].isNull()) {
    String ssid = d["wifi"]["ssid"] | "";
    String wp = d["wifi"]["pass"] | "";
    ssid.trim();
    if (ssid.length() == 0) { err = "WiFi name is required"; return false; }
    bool ssidChanged = ssid != n.wifiSsid;
    if (wp.length()) n.wifiPass = wp;
    else if (ssidChanged) n.wifiPass = "";          // new network, no password typed = open network
    if (ssidChanged || wp.length()) restart = true;
    n.wifiSsid = ssid;
  } else if (!n.wifiSsid.length()) {
    err = "WiFi name is required";
    return false;
  }
  if (!d["tz"].isNull()) n.tzMinutes = constrain((int)d["tz"], -720, 840);
  if (!d["syncHours"].isNull()) n.syncHours = constrain((int)d["syncHours"], 1, 24);
  if (!d["gh"].isNull()) {
    String tok = d["gh"]["token"] | "";
    n.ghOwner = String((const char*)(d["gh"]["owner"] | ""));
    n.ghRepo = String((const char*)(d["gh"]["repo"] | ""));
    String br = d["gh"]["branch"] | "main";
    n.ghBranch = br.length() ? br : "main";
    n.ghOwner.trim(); n.ghRepo.trim();
    if (tok.length()) n.ghToken = tok;     // blank token = keep the saved one
  }
  if (!d["opts"].isNull()) {
    n.dupSeconds = constrain((int)(d["opts"]["dup"] | 5), 1, 120);
    n.led = d["opts"]["led"] | true;
    n.dim = d["opts"]["dim"] | true;
    n.uploadDelaySec = constrain((int)(d["opts"]["uploadDelay"] | 90), 10, 3600);
  }

  if (setup && d["workers"].is<JsonArray>()) {
    std::vector<Worker> nw;
    for (JsonObject o : d["workers"].as<JsonArray>()) {
      Worker w;
      w.id = String((const char*)(o["id"] | ""));
      w.name = String((const char*)(o["name"] | ""));
      w.uid = normUid(String((const char*)(o["uid"] | "")));
      w.id.trim(); w.name.trim();
      if (!Storage::validId(w.id)) { err = "Bad worker ID: " + w.id; return false; }
      if (!Storage::validUid(w.uid)) { err = "Bad card ID for worker " + w.id; return false; }
      for (auto& x : nw)
        if (x.id == w.id || x.uid == w.uid) { err = "Duplicate worker ID or card: " + w.id; return false; }
      Worker* old = Storage::byId(w.id);
      if (old) { w.in = old->in; w.lastEpoch = old->lastEpoch; w.active = old->active; }
      nw.push_back(w);
    }
    auto backup = Storage::workers;
    Storage::workers = nw;
    if (!Storage::saveWorkers()) {
      Storage::workers = backup;
      err = "Could not write workers to the SD card - insert a FAT32 card and try again";
      return false;
    }
  }

  if (setup) n.configured = true;
  cfg = n;
  cfg.save();
  return true;
}

// ------------------------------------------------------------------ setup endpoints
static void hScan() {
  int n = WiFi.scanNetworks(false, false);
  JsonDocument d;
  JsonArray a = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (JsonObject o : a) if (s == (const char*)o["ssid"]) dup = true;
    if (dup) continue;
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void hWifiTest() {
  JsonDocument b;
  readBody(b);
  String ssid = b["ssid"] | "", pass = b["pass"] | "";
  if (!pass.length() && ssid == cfg.wifiSsid) pass = cfg.wifiPass;     // keep saved password
  if (!ssid.length()) { sendErr("Enter the WiFi name"); return; }
  WiFi.disconnect(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  wl_status_t st = WL_DISCONNECTED;
  while (millis() - t0 < 15000) {
    st = WiFi.status();
    if (st == WL_CONNECTED || st == WL_NO_SSID_AVAIL || st == WL_CONNECT_FAILED) break;
    delay(200);
  }
  if (st == WL_CONNECTED) {
    Clock::begin();
    JsonDocument d;
    d["ok"] = true;
    d["ip"] = WiFi.localIP().toString();
    sendJson(d);
    return;
  }
  WiFi.disconnect(false);
  if (st == WL_NO_SSID_AVAIL) sendErr("Network not found (is it a 2.4 GHz network and in range?)");
  else if (st == WL_CONNECT_FAILED) sendErr("Could not connect - wrong password?");
  else sendErr("Timed out - check the name and password");
}

static void hGhTest() {
  JsonDocument b;
  readBody(b);
  String owner = b["owner"] | "", repo = b["repo"] | "", br = b["branch"] | "main", tok = b["token"] | "";
  if (!tok.length() && owner == cfg.ghOwner && repo == cfg.ghRepo) tok = cfg.ghToken;
  if (WiFi.status() != WL_CONNECTED) { sendErr("Test the company WiFi first (the device needs internet)"); return; }
  uint32_t t0 = millis();
  while (!Clock::synced() && millis() - t0 < 10000) delay(200);       // HTTPS needs the time
  String msg;
  bool ok = Github::test(owner, repo, br, tok, msg);
  JsonDocument d;
  d["ok"] = ok;
  d["message"] = msg;
  sendJson(d);
}

static void hLastTap() {
  JsonDocument d;
  d["seq"] = (uint32_t)Nfc::tapSeq;
  d["uid"] = Nfc::lastUid;
  d["nfc"] = Nfc::ready();
  sendJson(d);
}

static void hSetup() {
  JsonDocument b;
  if (!readBody(b)) { sendErr("Bad JSON"); return; }
  String err;
  bool restart;
  if (!applySettings(b.as<JsonVariant>(), true, err, restart)) { sendErr(err); return; }
  sendOk();
  app_requestRestart(1500);
}

static void hCurrent() {
  JsonDocument d;
  settingsToJson(d, true);
  sendJson(d);
}

static void hSetTime() {
  JsonDocument b;
  readBody(b);
  uint32_t ep = b["epoch"] | 0;
  if (ep < 1700000000UL) { sendErr("Invalid time"); return; }
  Clock::setFromPhone((time_t)ep);
  sendOk();
}

// ------------------------------------------------------------------ admin endpoints
static void hStatus() {
  JsonDocument d;
  time_t now = Clock::now();
  d["company"] = cfg.company;
  d["deviceId"] = cfg.deviceId;
  d["fw"] = FW_VERSION;
  d["time"] = Clock::synced() ? Clock::hhmmss(now) : "--:--:--";
  d["date"] = Clock::synced() ? Clock::longDate(now) : "time not set";
  d["timeSynced"] = Clock::synced();
  d["timeTrusted"] = Clock::trusted();
  d["syncAgo"] = Clock::secondsSinceSync();
  d["sd"] = Storage::ok();
  d["queued"] = Storage::queuedCount();
  d["pending"] = Storage::pendingCount();
  d["nfc"] = Nfc::ready();
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  d["tapsToday"] = g_tapsToday;
  JsonObject g = d["github"].to<JsonObject>();
  g["state"] = Github::state();
  g["okAgo"] = Github::secondsSinceOk();
  g["error"] = Github::lastError();
  g["pendingDays"] = Github::pendingDays();
  JsonArray in = d["inside"].to<JsonArray>();
  String today = Clock::synced() ? Clock::day(now) : "";
  for (auto& w : Storage::workers)
    if (w.in && w.active && (!Clock::synced() || w.lastEpoch == 0 || Clock::day(w.lastEpoch) == today)) in.add(w.name);
  sendJson(d);
}

static void hWorkers() {
  JsonDocument d;
  JsonArray a = d["workers"].to<JsonArray>();
  for (auto& w : Storage::workers) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = w.id; o["uid"] = w.uid; o["name"] = w.name; o["active"] = w.active; o["in"] = w.in;
  }
  d["sd"] = Storage::ok();
  sendJson(d);
}

static void hWorkerUpsert() {
  JsonDocument b;
  if (!readBody(b)) { sendErr("Bad JSON"); return; }
  if (!Storage::ok()) { sendErr("SD card missing - cannot save"); return; }
  String orig = b["origId"] | "";
  Worker w;
  w.id = String((const char*)(b["id"] | ""));
  w.name = String((const char*)(b["name"] | ""));
  w.uid = normUid(String((const char*)(b["uid"] | "")));
  w.active = b["active"] | true;
  w.id.trim(); w.name.trim();
  if (!Storage::validId(w.id)) { sendErr("ID must be 1-16 letters, digits or . _ -"); return; }
  if (w.name.length() == 0) { sendErr("Name is required"); return; }
  if (!Storage::validUid(w.uid)) { sendErr("Card ID must be 8+ hex characters (use Tap card now)"); return; }
  for (auto& x : Storage::workers) {
    if (orig.length() && x.id == orig) continue;
    if (x.id == w.id) { sendErr("Another worker already has ID " + w.id); return; }
    if (x.uid == w.uid) { sendErr("That card already belongs to " + x.name); return; }
  }
  auto backup = Storage::workers;
  Worker* old = orig.length() ? Storage::byId(orig) : nullptr;
  if (orig.length() && !old) { sendErr("Worker not found"); return; }
  if (old) { w.in = old->in; w.lastEpoch = old->lastEpoch; w.workedToday = old->workedToday; w.workedDay = old->workedDay; *old = w; }
  else Storage::workers.push_back(w);
  if (!Storage::saveWorkers()) { Storage::workers = backup; sendErr("SD write failed"); return; }
  for (auto it = Storage::unknown.begin(); it != Storage::unknown.end(); ++it)
    if (it->uid == w.uid) { Storage::unknown.erase(it); break; }
  sendOk();
}

static void hWorkerDelete() {
  JsonDocument b;
  readBody(b);
  String id = b["id"] | "";
  auto backup = Storage::workers;
  bool found = false;
  for (auto it = Storage::workers.begin(); it != Storage::workers.end(); ++it)
    if (it->id == id) { Storage::workers.erase(it); found = true; break; }
  if (!found) { sendErr("Worker not found", 404); return; }
  if (!Storage::saveWorkers()) { Storage::workers = backup; sendErr("SD write failed"); return; }
  sendOk();
}

static void hWorkersCsv() {
  String s = "\xEF\xBB\xBF" "employee_id,uid,name,active\n";
  for (auto& w : Storage::workers)
    s += Storage::csvField(w.id) + "," + w.uid + "," + Storage::csvField(w.name) + "," + (w.active ? "1" : "0") + "\n";
  server.sendHeader("Content-Disposition", "attachment; filename=\"muhax-workers.csv\"");
  server.send(200, "text/csv; charset=utf-8", s);
}

static void hWorkersImport() {
  JsonDocument b;
  if (!readBody(b)) { sendErr("Bad JSON"); return; }
  if (!Storage::ok()) { sendErr("SD card missing"); return; }
  String csv = b["csv"] | "";
  if (csv.startsWith("\xEF\xBB\xBF")) csv = csv.substring(3);
  auto backup = Storage::workers;
  int added = 0, updated = 0, start = 0, lineNo = 0;
  while (start < (int)csv.length()) {
    int nl = csv.indexOf('\n', start);
    if (nl < 0) nl = csv.length();
    String line = csv.substring(start, nl);
    start = nl + 1;
    lineNo++;
    line.trim();
    if (!line.length()) continue;
    std::vector<String> f;
    Storage::splitCsv(line, f);
    if (lineNo == 1 && f[0].equalsIgnoreCase("employee_id")) continue;
    if (f.size() < 3) { Storage::workers = backup; sendErr("Line " + String(lineNo) + ": need employee_id,uid,name"); return; }
    Worker w;
    w.id = f[0]; w.id.trim();
    w.uid = normUid(f[1]);
    w.name = f[2]; w.name.trim();
    w.active = f.size() < 4 || f[3] != "0";
    if (!Storage::validId(w.id) || !Storage::validUid(w.uid) || !w.name.length()) {
      Storage::workers = backup; sendErr("Line " + String(lineNo) + ": invalid ID, card or name"); return;
    }
    Worker* other = Storage::byUid(w.uid);
    if (other && other->id != w.id) { Storage::workers = backup; sendErr("Line " + String(lineNo) + ": card already used by " + other->id); return; }
    Worker* ex = Storage::byId(w.id);
    if (ex) { ex->uid = w.uid; ex->name = w.name; ex->active = w.active; updated++; }
    else { Storage::workers.push_back(w); added++; }
  }
  if (!Storage::saveWorkers()) { Storage::workers = backup; sendErr("SD write failed"); return; }
  JsonDocument d;
  d["ok"] = true; d["added"] = added; d["updated"] = updated;
  sendJson(d);
}

static void hUnknown() {
  JsonDocument d;
  JsonArray a = d["cards"].to<JsonArray>();
  for (auto& c : Storage::unknown) {
    JsonObject o = a.add<JsonObject>();
    o["uid"] = c.uid;
    o["last"] = c.last ? Clock::fmt(c.last, "%Y-%m-%d %H:%M:%S") : "-";
    o["count"] = c.count;
  }
  sendJson(d);
}

static void hDates() {
  JsonDocument d;
  JsonArray a = d["days"].to<JsonArray>();
  for (auto& s : Storage::listDays()) a.add(s);
  sendJson(d);
}

static void hRecords() {
  String from = server.arg("from"), to = server.arg("to"), worker = server.arg("worker");
  bool bom = server.arg("bom") == "1", dl = server.arg("dl") == "1";
  if (!Clock::isValidDay(from) || !Clock::isValidDay(to)) { sendErr("from/to must be YYYY-MM-DD"); return; }
  if (dl) server.sendHeader("Content-Disposition", "attachment; filename=\"muhax-" + from + "_" + to + ".csv\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  String out;
  if (bom) out += "\xEF\xBB\xBF";
  out += "timestamp_local,epoch_utc,uid,employee_id,name,action,time_trusted,time_corrected,device_id,flag\n";
  for (const String& d : Storage::listDays()) {
    if (d < from || d > to) continue;
    String path = Storage::logPath(d);
    size_t off = 0;
    String chunk;
    while (true) {
      size_t n = Storage::readChunk(path, off, chunk, 2048);
      if (n == off) break;
      off = n;
      int s = 0;
      while (s < (int)chunk.length()) {
        int nl = chunk.indexOf('\n', s);
        if (nl < 0) nl = chunk.length();
        String line = chunk.substring(s, nl);
        s = nl + 1;
        if (!line.length() || line.startsWith("timestamp_local")) continue;
        if (worker.length()) {
          std::vector<String> f;
          Storage::splitCsv(line, f);
          if (f.size() < 4 || f[3] != worker) continue;
        }
        out += line;
        out += '\n';
        if (out.length() > 1400) { server.sendContent(out); out = ""; }
      }
      yield();
    }
  }
  server.sendContent(out);
  server.sendContent("");
}

static void hSettingsGet() {
  JsonDocument d;
  settingsToJson(d, false);
  sendJson(d);
}

static void hSettingsPost() {
  JsonDocument b;
  if (!readBody(b)) { sendErr("Bad JSON"); return; }
  String err;
  bool restart;
  if (!applySettings(b.as<JsonVariant>(), false, err, restart)) { sendErr(err); return; }
  Clock::begin();                                   // re-apply the sync interval
  JsonDocument d;
  d["ok"] = true;
  d["restart"] = restart;
  sendJson(d);
  if (restart) app_requestRestart(1500);
}

static void hPassword() {
  JsonDocument b;
  readBody(b);
  String user = b["user"] | "", oldp = b["old"] | "", np = b["pass"] | "";
  user.trim();
  if (!cfg.checkAdmin(cfg.adminUser, oldp)) { sendErr("Current password is wrong", 403); return; }
  if (user.length() < 3) { sendErr("Username needs 3+ characters"); return; }
  if (np.length() && np.length() < 8) { sendErr("New password needs 8+ characters"); return; }
  cfg.adminUser = user;
  if (np.length()) cfg.setAdminPassword(np);
  cfg.save();
  sendOk();
}

static void hSyncGithub() { Github::kick(); sendOk(); }
static void hSyncTime() { Clock::requestSync(); sendOk(); }
static void hRestart() { sendOk(); app_requestRestart(800); }
static void hSetupMode() { sendOk(); app_enterSetupMode(); }
static void hFactory() {
  JsonDocument b;
  readBody(b);
  if (String((const char*)(b["confirm"] | "")) != "RESET") { sendErr("Confirmation missing"); return; }
  cfg.factoryReset();
  sendOk();
  app_requestRestart(1200);
}

// ------------------------------------------------------------------ pages
static void sendPage(const char* head, const char* js, const char* tail) {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(strlen_P(head) + strlen_P(js) + strlen_P(tail));
  server.send(200, "text/html", "");
  server.sendContent_P(head);
  server.sendContent_P(js);
  server.sendContent_P(tail);
}

static void hRoot() {
  if (fromAp()) { sendPage(SETUP_HTML_HEAD, SETUP_JS, SETUP_HTML_TAIL); return; }
  if (!needAdmin()) return;
  sendPage(ADMIN_HTML_HEAD, ADMIN_JS, ADMIN_HTML_TAIL);
}

static void hNotFound() {
  if (fromAp()) {            // captive portal: send everything to the setup page
    server.sendHeader("Location", "http://" SETUP_IP_STR "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found");
}

void begin() {
  const char* hdr[] = {"Authorization"};
  server.collectHeaders(hdr, 1);
  server.on("/", HTTP_GET, hRoot);

  // setup network only
  setupRoute("/api/current", HTTP_GET, hCurrent);
  setupRoute("/api/scan", HTTP_GET, hScan);
  setupRoute("/api/wifitest", HTTP_POST, hWifiTest);
  setupRoute("/api/setup", HTTP_POST, hSetup);
  // either
  server.on("/api/ghtest", HTTP_POST, []() { if (needAny()) hGhTest(); });
  server.on("/api/lasttap", HTTP_GET, []() { if (needAny()) hLastTap(); });
  server.on("/api/settime", HTTP_POST, []() { if (needAny()) hSetTime(); });

  // admin (login) only
  adminRoute("/api/status", HTTP_GET, hStatus);
  adminRoute("/api/workers", HTTP_GET, hWorkers);
  adminRoute("/api/worker", HTTP_POST, hWorkerUpsert);
  adminRoute("/api/worker/delete", HTTP_POST, hWorkerDelete);
  adminRoute("/api/workers/import", HTTP_POST, hWorkersImport);
  adminRoute("/workers.csv", HTTP_GET, hWorkersCsv);
  adminRoute("/api/unknown", HTTP_GET, hUnknown);
  adminRoute("/api/dates", HTTP_GET, hDates);
  adminRoute("/records.csv", HTTP_GET, hRecords);
  adminRoute("/api/settings", HTTP_GET, hSettingsGet);
  adminRoute("/api/settings", HTTP_POST, hSettingsPost);
  adminRoute("/api/password", HTTP_POST, hPassword);
  adminRoute("/api/sync-github", HTTP_POST, hSyncGithub);
  adminRoute("/api/sync-time", HTTP_POST, hSyncTime);
  adminRoute("/api/restart", HTTP_POST, hRestart);
  adminRoute("/api/setup-mode", HTTP_POST, hSetupMode);
  adminRoute("/api/factory-reset", HTTP_POST, hFactory);
  server.onNotFound(hNotFound);
}

// Must only be called after WiFi has been started (AP or STA), otherwise the TCP/IP stack is not ready.
static bool gServerStarted = false;
void startServer() {
  if (gServerStarted) return;
  server.begin();
  gServerStarted = true;
}

void loop() {
  if (gAp) dns.processNextRequest();
  if (gServerStarted) server.handleClient();
}

}  // namespace Web
