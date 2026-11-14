#include "github.h"
#include "settings.h"
#include "storage.h"
#include "clock.h"
#include "github_ca.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <mbedtls/base64.h>

namespace Github {

static const char* F_SYNC = "/muhax/sync.json";
static const size_t MAX_UPLOAD = 48 * 1024;

static volatile bool gKick = false;
static volatile uint32_t gLastOkMs = 0;
static volatile bool gEverOk = false;
static String gErr;
static volatile int gPending = 0;

// sync bookkeeping: path -> {sha, sig}
struct Entry { String path, sha, sig; };
static std::vector<Entry> gSync;

static void loadSync() {
  gSync.clear();
  String txt;
  if (!Storage::readFile(F_SYNC, txt, 64 * 1024)) return;
  JsonDocument doc;
  if (deserializeJson(doc, txt)) return;
  for (JsonPair kv : doc.as<JsonObject>())
    gSync.push_back({String(kv.key().c_str()), kv.value()["sha"] | "", kv.value()["sig"] | ""});
}
static void saveSync() {
  JsonDocument doc;
  for (auto& e : gSync) {
    doc[e.path]["sha"] = e.sha;
    doc[e.path]["sig"] = e.sig;
  }
  String s;
  serializeJson(doc, s);
  Storage::writeFile(F_SYNC, s);
}
static Entry* findEntry(const String& path) {
  for (auto& e : gSync) if (e.path == path) return &e;
  return nullptr;
}

// ---------------------------------------------------------------- HTTP
static String urlEncodePath(const String& p) { return p; }   // paths here are plain ASCII

static bool apiCall(const char* method, const String& url, const String& token, const String& body, int& code,
                    String& resp) {
  WiFiClientSecure client;
  client.setCACert(GITHUB_ROOT_CA);
  client.setTimeout(15);
  HTTPClient http;
  http.setReuse(false);
  http.setConnectTimeout(12000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) { resp = "begin failed"; code = -1; return false; }
  http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  http.addHeader("User-Agent", "muhax-logger");
  if (body.length()) http.addHeader("Content-Type", "application/json");
  code = http.sendRequest(method, (uint8_t*)body.c_str(), body.length());
  if (code > 0) resp = http.getString();
  else resp = HTTPClient::errorToString(code);
  http.end();
  return code > 0;
}

bool test(const String& owner, const String& repo, const String& branch, const String& token, String& message) {
  if (!owner.length() || !repo.length() || !token.length()) { message = "Owner, repo and token are required"; return false; }
  if (!Clock::synced()) { message = "Time not synced yet (needed for HTTPS)"; return false; }
  int code;
  String resp;
  if (!apiCall("GET", "https://api.github.com/repos/" + owner + "/" + repo, token, "", code, resp)) {
    message = "Connection failed: " + resp;
    return false;
  }
  if (code == 404) { message = "Repo not found (or the token cannot see it)"; return false; }
  if (code == 401) { message = "Token rejected (expired or wrong)"; return false; }
  if (code != 200) { message = "GitHub answered HTTP " + String(code); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) { message = "Unexpected answer from GitHub"; return false; }
  bool priv = doc["private"] | false;
  bool push = doc["permissions"]["push"] | false;
  if (!push) { message = "Token cannot write to this repo (needs Contents: read and write)"; return false; }
  if (branch.length()) {
    if (!apiCall("GET", "https://api.github.com/repos/" + owner + "/" + repo + "/branches/" + branch, token, "", code, resp) ||
        code != 200) {
      message = "Branch '" + branch + "' not found";
      return false;
    }
  }
  message = priv ? "OK - private repo, write access confirmed" : "OK, but the repo is PUBLIC - make it private!";
  return true;
}

// ---------------------------------------------------------------- upload
static String b64(const String& in) {
  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen, (const uint8_t*)in.c_str(), in.length());
  char* buf = (char*)malloc(olen + 1);
  if (!buf) return "";
  mbedtls_base64_encode((uint8_t*)buf, olen + 1, &olen, (const uint8_t*)in.c_str(), in.length());
  buf[olen] = 0;
  String s(buf);
  free(buf);
  return s;
}

static String contentsUrl(const String& path) {
  return "https://api.github.com/repos/" + cfg.ghOwner + "/" + cfg.ghRepo + "/contents/" + urlEncodePath(path);
}

static bool fetchSha(const String& path, String& sha) {
  int code;
  String resp;
  if (!apiCall("GET", contentsUrl(path) + "?ref=" + cfg.ghBranch, cfg.ghToken, "", code, resp)) { gErr = resp; return false; }
  if (code == 404) { sha = ""; return true; }
  if (code != 200) { gErr = "HTTP " + String(code) + " reading " + path; return false; }
  JsonDocument filter;
  filter["sha"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) { gErr = "bad JSON"; return false; }
  sha = doc["sha"] | "";
  return true;
}

// Uploads a whole file (create or update). `sig` identifies the version that was uploaded.
static bool uploadFile(const String& repoPath, const String& localPath, const String& sig) {
  String data;
  if (!Storage::readFile(localPath, data, MAX_UPLOAD)) { gErr = "file too large or unreadable: " + localPath; return false; }
  String content = b64(data);
  if (!content.length() && data.length()) { gErr = "out of memory"; return false; }
  data = "";

  Entry* e = findEntry(repoPath);
  String sha = e ? e->sha : "";
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt == 1 || sha.length() == 0) {
      if (!fetchSha(repoPath, sha)) return false;
    }
    JsonDocument doc;
    doc["message"] = "MUHAX " + cfg.deviceId + " " + repoPath;
    doc["content"] = content;
    doc["branch"] = cfg.ghBranch;
    if (sha.length()) doc["sha"] = sha;
    String body;
    serializeJson(doc, body);
    int code;
    String resp;
    if (!apiCall("PUT", contentsUrl(repoPath), cfg.ghToken, body, code, resp)) { gErr = resp; return false; }
    if (code == 200 || code == 201) {
      JsonDocument filter;
      filter["content"]["sha"] = true;
      JsonDocument r;
      String newSha;
      if (!deserializeJson(r, resp, DeserializationOption::Filter(filter))) newSha = r["content"]["sha"] | "";
      if (!e) { gSync.push_back({repoPath, newSha, sig}); }
      else { e->sha = newSha; e->sig = sig; }
      saveSync();
      return true;
    }
    if (code == 409 || code == 422) { sha = ""; continue; }       // stale sha: re-read and retry once
    if (code == 401 || code == 403) gErr = "Token rejected/expired (HTTP " + String(code) + ")";
    else gErr = "HTTP " + String(code) + " uploading " + repoPath;
    return false;
  }
  gErr = "conflict uploading " + repoPath;
  return false;
}

static String sizeSig(const String& path) { return String((unsigned long)Storage::fileSize(path)); }

static String workersSig() {
  String t;
  if (!Storage::readFile("/muhax/workers.csv", t, 64 * 1024)) return "";
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < t.length(); i++) { h ^= (uint8_t)t[i]; h *= 16777619u; }
  return String(t.length()) + "-" + String(h, HEX);
}

struct Job { String repoPath, localPath, sig; };

static void collectJobs(std::vector<Job>& jobs) {
  jobs.clear();
  String dev = cfg.deviceId;
  for (const String& d : Storage::listDays()) {
    String lp = Storage::logPath(d);
    String sig = sizeSig(lp);
    String rp = "attendance/" + dev + "/" + d + ".csv";
    Entry* e = findEntry(rp);
    if (sig != "0" && (!e || e->sig != sig)) jobs.push_back({rp, lp, sig});
  }
  String ws = workersSig();
  Entry* we = findEntry("config/workers-" + dev + ".csv");
  if (ws.length() && (!we || we->sig != ws)) jobs.push_back({"config/workers-" + dev + ".csv", "/muhax/workers.csv", ws});
}

static void task(void*) {
  uint32_t lastScan = 0;
  bool loaded = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(2000));
    if (!cfg.configured || !cfg.githubConfigured()) { gPending = 0; continue; }
    if (!Storage::ok() || WiFi.status() != WL_CONNECTED || !Clock::synced()) continue;
    if (!loaded) { loadSync(); loaded = true; }
    bool force = gKick;
    uint32_t quiet = millis() - Storage::lastWriteMs;
    if (!force && millis() - lastScan < 20000) continue;
    lastScan = millis();
    std::vector<Job> jobs;
    collectJobs(jobs);
    gPending = jobs.size();
    if (jobs.empty()) { if (!gEverOk || force) { gEverOk = true; gLastOkMs = millis(); gErr = ""; } gKick = false; gLastOkMs = millis(); continue; }
    if (!force && quiet < (uint32_t)cfg.uploadDelaySec * 1000UL) continue;   // batch taps into one commit
    gKick = false;
    for (auto& j : jobs) {
      if (WiFi.status() != WL_CONNECTED) break;
      if (uploadFile(j.repoPath, j.localPath, j.sig)) { gErr = ""; gLastOkMs = millis(); gEverOk = true; gPending--; }
      else break;                 // retry on the next scan
      vTaskDelay(pdMS_TO_TICKS(300));
    }
  }
}

void begin() { xTaskCreatePinnedToCore(task, "gh", 12288, nullptr, 1, nullptr, 0); }
void kick() { gKick = true; }

int state() {
  if (!cfg.githubConfigured()) return 0;
  if (gErr.length() || gPending > 0) return 2;
  return gEverOk ? 1 : 2;
}
uint32_t secondsSinceOk() { return gEverOk ? (millis() - gLastOkMs) / 1000UL : 0xFFFFFFFFUL; }
String lastError() { return gErr; }
int pendingDays() { return gPending; }

}  // namespace Github
