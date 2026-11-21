// MUHAX Logger QR - ESP32-2432S028R "CYD"
// Phone-based check-in/out: rotating QR on screen -> local web page -> SD card + GitHub.
// See README.md for build/test notes.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>
#include <ff.h>
#include <TFT_eSPI.h>
#include <qrcode.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <vector>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <esp_system.h>
#include <esp_sntp.h>

// ---------------------------------------------------------------- pins
#define PIN_LED_R 4
#define PIN_LED_G 16
#define PIN_LED_B 17
#define PIN_BOOT 0
#define SD_CS 5
#define SD_SCK 18
#define SD_MISO 19
#define SD_MOSI 23

// Set to 1 to skip GitHub certificate checking (not recommended). See README.
#define GITHUB_INSECURE_TLS 1

static const char* AP_SSID = "Muhax_logger";
static const char* MDNS_NAME = "muhax-logger";

// ---------------------------------------------------------------- globals
TFT_eSPI tft;
SPIClass sdSPI(VSPI);
WebServer server(80);
DNSServer dns;
Preferences prefs;
static bool sdOk = false;
static bool setupMode = false;
static volatile bool maintenanceMode = false;
static String devId;
static String apPass;
static uint32_t restartAt = 0;

struct Cfg {
  String ssid, wpass, company, au, asalt, ah, gho, ghr, ghb, ght, qrs, cks;
  int tzh = 3, syncH = 2, dbl = 5, qrr = 30, ghDelay = 90;
} cfg;

struct Worker {
  String id, name, salt, hash;
  bool active = true;
  int gen = 0;
  int lastAct = 0;  // 0 none, 1 IN, 2 OUT
  time_t lastEpoch = 0;
  int fails = 0;
  uint32_t lockUntil = 0;
};
static std::vector<Worker> workers;

// locks: sdM guards SD + workers vector writes; pendM guards the upload queue
static SemaphoreHandle_t sdM, pendM;
struct Lock {
  SemaphoreHandle_t m;
  explicit Lock(SemaphoreHandle_t x) : m(x) { xSemaphoreTake(m, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m); }
};

// time
static volatile uint32_t lastSyncMs = 0;
static bool resyncNow = false;
static uint32_t lastResync = 0;

// github queue
static String pend[8];
static int npend = 0;
static uint32_t lastChange = 0;
static bool workersDirty = false;
static bool databaseUploadRequested = false;
static volatile uint32_t lastGhOkMs = 0;
static volatile bool ghLastOk = false;

// ui
enum Ui { UI_IDLE, UI_RESULT, UI_INFO };
static Ui ui = UI_IDLE;
static uint32_t uiUntil = 0;
static bool needFull = true;
static long lastWin = -1;
static int lastSec = -1;
static String lastLine = "";
static uint32_t wifiLostSince = 0;
static uint32_t btnDown = 0;

// ---------------------------------------------------------------- helpers
static String toHex(const uint8_t* b, size_t n) {
  String s;
  s.reserve(n * 2);
  char t[3];
  for (size_t i = 0; i < n; i++) { sprintf(t, "%02x", b[i]); s += t; }
  return s;
}
static String sha256hex(const String& s) {
  uint8_t o[32];
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)s.c_str(), s.length(), o);
  return toHex(o, 32);
}
static String hmacHex(const String& key, const String& msg) {
  uint8_t o[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const uint8_t*)key.c_str(), key.length(),
                  (const uint8_t*)msg.c_str(), msg.length(), o);
  return toHex(o, 32);
}
static String randHex(int n) {
  String s;
  char t[9];
  while ((int)s.length() < n) { sprintf(t, "%08x", (unsigned)esp_random()); s += t; }
  return s.substring(0, n);
}
static String randPin() {
  char b[8];
  sprintf(b, "%04u", (unsigned)(esp_random() % 10000));
  return String(b);
}
static String b64enc(const String& in) {
  size_t ol = 0;
  mbedtls_base64_encode(nullptr, 0, &ol, (const uint8_t*)in.c_str(), in.length());
  uint8_t* o = (uint8_t*)malloc(ol + 1);
  if (!o) return "";
  mbedtls_base64_encode(o, ol, &ol, (const uint8_t*)in.c_str(), in.length());
  o[ol] = 0;
  String r((char*)o);
  free(o);
  return r;
}
static String b64dec(const String& in) {
  size_t ol = 0;
  mbedtls_base64_decode(nullptr, 0, &ol, (const uint8_t*)in.c_str(), in.length());
  uint8_t* o = (uint8_t*)malloc(ol + 1);
  if (!o) return "";
  if (mbedtls_base64_decode(o, ol, &ol, (const uint8_t*)in.c_str(), in.length()) != 0) { free(o); return ""; }
  o[ol] = 0;
  String r((char*)o);
  free(o);
  return r;
}
static String esc(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}
static String cleanField(String s) {  // for CSV fields
  s.replace(",", " ");
  s.replace("\"", "");
  s.replace("\r", "");
  s.replace("\n", " ");
  s.trim();
  return s;
}
static bool validId(const String& s) {
  if (!s.length() || s.length() > 12) return false;
  for (unsigned i = 0; i < s.length(); i++)
    if (!isalnum((unsigned char)s[i])) return false;
  return true;
}
static bool validPin(const String& s) {
  if (s.length() < 4 || s.length() > 6) return false;
  for (unsigned i = 0; i < s.length(); i++)
    if (!isdigit((unsigned char)s[i])) return false;
  return true;
}

// ---------------------------------------------------------------- time
static bool timeOk() { return time(nullptr) > 1700000000; }
static String fmtTime(time_t t, const char* f) {
  struct tm tm;
  localtime_r(&t, &tm);
  char b[48];
  strftime(b, sizeof b, f, &tm);
  return String(b);
}
static bool sameDay(time_t a, time_t b) { return fmtTime(a, "%Y%m%d") == fmtTime(b, "%Y%m%d"); }
static bool timeTrusted() { return timeOk() && lastSyncMs != 0 && (millis() - lastSyncMs) < 24UL * 3600UL * 1000UL; }
static void onSync(struct timeval*) { lastSyncMs = millis() ? millis() : 1; }
static void startNtp() {
  String tz = String("UTC") + (cfg.tzh >= 0 ? "-" : "+") + String(abs(cfg.tzh));
  configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");
  lastResync = millis();
}

// ---------------------------------------------------------------- config
static void loadCfg() {
  prefs.begin("mx", true);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.wpass = prefs.getString("wpass", "");
  cfg.company = prefs.getString("co", "");
  cfg.au = prefs.getString("au", "");
  cfg.asalt = prefs.getString("as", "");
  cfg.ah = prefs.getString("ah", "");
  cfg.gho = prefs.getString("gho", "");
  cfg.ghr = prefs.getString("ghr", "");
  cfg.ghb = prefs.getString("ghb", "main");
  cfg.ght = prefs.getString("ght", "");
  cfg.qrs = prefs.getString("qrs", "");
  cfg.cks = prefs.getString("cks", "");
  cfg.tzh = prefs.getInt("tz", 3);
  cfg.syncH = prefs.getInt("syncH", 2);
  cfg.dbl = prefs.getInt("dbl", 5);
  cfg.qrr = prefs.getInt("qrr", 30);
  cfg.ghDelay = prefs.getInt("ghd", 90);
  prefs.end();
}
static void saveCfg() {
  prefs.begin("mx", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("wpass", cfg.wpass);
  prefs.putString("co", cfg.company);
  prefs.putString("au", cfg.au);
  prefs.putString("as", cfg.asalt);
  prefs.putString("ah", cfg.ah);
  prefs.putString("gho", cfg.gho);
  prefs.putString("ghr", cfg.ghr);
  prefs.putString("ghb", cfg.ghb);
  prefs.putString("ght", cfg.ght);
  prefs.putString("qrs", cfg.qrs);
  prefs.putString("cks", cfg.cks);
  prefs.putInt("tz", cfg.tzh);
  prefs.putInt("syncH", cfg.syncH);
  prefs.putInt("dbl", cfg.dbl);
  prefs.putInt("qrr", cfg.qrr);
  prefs.putInt("ghd", cfg.ghDelay);
  prefs.end();
}
static bool configured() { return cfg.ssid.length() && cfg.au.length() && cfg.ah.length(); }

// ---------------------------------------------------------------- workers (SD)
static int splitCsv(const String& line, String* out, int n) {
  int c = 0, st = 0;
  for (int i = 0; i <= (int)line.length() && c < n; i++) {
    if (i == (int)line.length() || line[i] == ',') { out[c++] = line.substring(st, i); st = i + 1; }
  }
  return c;
}
static Worker* findW(const String& id) {
  for (auto& w : workers) if (w.id == id) return &w;
  return nullptr;
}
static void loadWorkers() {
  Lock l(sdM);
  workers.clear();
  if (!sdOk) return;
  File f = SD.open("/muhax/workers.csv");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length() || line.startsWith("employee_id")) continue;
    String p[8];
    if (splitCsv(line, p, 8) < 8) continue;
    Worker w;
    w.id = p[0]; w.name = p[1]; w.salt = p[2]; w.hash = p[3];
    w.active = p[4] == "1"; w.gen = p[5].toInt(); w.lastAct = p[6].toInt();
    w.lastEpoch = (time_t)p[7].toInt();
    workers.push_back(w);
  }
  f.close();
}
static bool saveWorkers() {
  Lock l(sdM);
  if (!sdOk) return false;
  SD.remove("/muhax/workers.tmp");
  File f = SD.open("/muhax/workers.tmp", FILE_WRITE);
  if (!f) return false;
  f.println("employee_id,name,salt,pin_hash,active,gen,last_act,last_epoch");
  for (auto& w : workers) {
    f.printf("%s,%s,%s,%s,%d,%d,%d,%ld\n", w.id.c_str(), w.name.c_str(), w.salt.c_str(), w.hash.c_str(),
             w.active ? 1 : 0, w.gen, w.lastAct, (long)w.lastEpoch);
  }
  f.close();
  SD.remove("/muhax/workers.csv");
  SD.rename("/muhax/workers.tmp", "/muhax/workers.csv");
  workersDirty = true;
  return true;
}
static void setPin(Worker& w, const String& pin) {
  w.salt = randHex(8);
  w.hash = sha256hex(w.salt + pin);
  w.fails = 0;
  w.lockUntil = 0;
}
static int insideCount() {
  int n = 0;
  time_t now = time(nullptr);
  for (auto& w : workers) if (w.active && w.lastAct == 1 && sameDay(w.lastEpoch, now)) n++;
  return n;
}

// ---------------------------------------------------------------- SD helpers
static String readAll(const String& path) {
  Lock l(sdM);
  String s;
  if (!sdOk) return s;
  File f = SD.open(path);
  if (!f) return s;
  s.reserve(f.size() + 1);
  uint8_t buf[256];
  int n;
  while ((n = f.read(buf, sizeof buf)) > 0) s.concat((const char*)buf, n);
  f.close();
  return s;
}
static void markDirty(const String& day) {
  Lock l(pendM);
  bool have = false;
  for (int i = 0; i < npend; i++) if (pend[i] == day) have = true;
  if (!have && npend < 8) pend[npend++] = day;
  lastChange = millis();
}
static void requestDatabaseUpload() {
  Lock l(pendM);
  databaseUploadRequested = true;
  lastChange = 0;
}
static bool clearDirectory(const String& path) {
  if (!SD.exists(path)) return true;
  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }

  struct Entry {
    String path;
    bool directory;
  };
  std::vector<Entry> entries;
  File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    bool directory = entry.isDirectory();
    entry.close();
    if (!name.length()) {
      dir.close();
      return false;
    }
    if (name.length() && name != "." && name != "..")
      entries.push_back({path + "/" + name, directory});
    entry = dir.openNextFile();
  }
  dir.close();

  for (const Entry& child : entries) {
    if (child.directory) {
      if (!clearDirectory(child.path) || !SD.rmdir(child.path)) return false;
    } else if (!SD.remove(child.path)) {
      return false;
    }
  }
  return true;
}
static bool formatSdCard(String& error) {
  maintenanceMode = true;
  bool formatted = false;
  {
    Lock l(sdM);
    if (!sdOk) {
      error = "No SD card is available.";
    } else {
      uint8_t work[FF_MAX_SS];
      // This firmware mounts its SD card as the first and only FAT volume.
      f_mount(nullptr, "0:", 0);
      FRESULT result = f_mkfs("0:", FM_ANY, 0, work, sizeof(work));
      SD.end();
      sdOk = SD.begin(SD_CS, sdSPI) && SD.cardType() != CARD_NONE;
      if (result != FR_OK) {
        error = "The SD card could not be formatted (FatFs error " + String((int)result) + ").";
      } else if (!sdOk) {
        error = "The card was formatted, but could not be mounted again.";
      } else if (!SD.mkdir("/muhax") || !SD.mkdir("/muhax/logs")) {
        error = "The card was formatted, but the logger folders could not be created.";
      } else {
        workers.clear();
        formatted = true;
      }
    }
  }
  if (formatted) {
    Lock l(pendM);
    npend = 0;
    workersDirty = false;
    databaseUploadRequested = false;
  } else {
    maintenanceMode = false;
  }
  return formatted;
}
static bool factoryReset(String& error) {
  maintenanceMode = true;
  {
    Lock l(sdM);
    if (!sdOk) {
      error = "Factory reset requires an available SD card so worker and attendance data can be erased.";
      maintenanceMode = false;
      return false;
    }
    if ((SD.exists("/muhax/workers.csv") && !SD.remove("/muhax/workers.csv")) ||
        (SD.exists("/muhax/workers.tmp") && !SD.remove("/muhax/workers.tmp")) ||
        !clearDirectory("/muhax/logs")) {
      error = "Could not erase all worker and attendance files. Settings were not reset.";
      maintenanceMode = false;
      return false;
    }
    workers.clear();
  }

  bool opened = prefs.begin("mx", false);
  bool cleared = opened && prefs.clear();
  if (opened) prefs.end();
  if (!cleared) {
    error = "Worker and attendance data were erased, but device settings could not be reset.";
    maintenanceMode = false;
    return false;
  }
  {
    Lock l(pendM);
    npend = 0;
    workersDirty = false;
    databaseUploadRequested = false;
  }
  restartAt = millis() + 1500;
  return true;
}
static bool appendLog(const Worker& w, const char* action, const String& phone) {
  time_t now = time(nullptr);
  String path = "/muhax/logs/" + fmtTime(now, "%Y-%m-%d") + ".csv";
  Lock l(sdM);
  if (!sdOk) return false;
  bool isNew = !SD.exists(path);
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  if (isNew) f.println("timestamp_local,epoch_utc,employee_id,name,action,method,phone_id,time_trusted,device_id");
  f.printf("%s,%ld,%s,%s,%s,qr,%s,%d,%s\n", fmtTime(now, "%Y-%m-%d %H:%M:%S").c_str(), (long)now, w.id.c_str(),
           w.name.c_str(), action, phone.c_str(), timeTrusted() ? 1 : 0, devId.c_str());
  f.close();  // closed on every record: a power cut loses at most the record in progress
  return true;
}

// ---------------------------------------------------------------- GitHub
static void ghHeaders(HTTPClient& h, const String& token) {
  h.addHeader("Authorization", "Bearer " + token);
  h.addHeader("Accept", "application/vnd.github+json");
  h.addHeader("X-GitHub-Api-Version", "2022-11-28");
  h.setUserAgent("muhax-logger");
}
static void ghHeaders(HTTPClient& h) { ghHeaders(h, cfg.ght); }
static bool ghPut(const String& path, const String& content) {
  if (!cfg.ght.length() || !cfg.gho.length() || !cfg.ghr.length()) return false;
  WiFiClientSecure c;
#if GITHUB_INSECURE_TLS
  c.setInsecure();
#endif
  c.setTimeout(15);
  String url = "https://api.github.com/repos/" + cfg.gho + "/" + cfg.ghr + "/contents/" + path;
  String sha;
  {
    HTTPClient h;
    h.useHTTP10(true);  // no chunking, so the JSON can be streamed through a filter
    h.setTimeout(15000);
    if (!h.begin(c, url + "?ref=" + cfg.ghb)) return false;
    ghHeaders(h);
    int code = h.GET();
    if (code == 200) {
      JsonDocument filter;
      filter["sha"] = true;
      JsonDocument doc;
      if (!deserializeJson(doc, h.getStream(), DeserializationOption::Filter(filter))) sha = doc["sha"] | "";
    } else if (code != 404) {
      h.end();
      return false;
    }
    h.end();
  }
  c.stop();
  JsonDocument d;
  d["message"] = "log " + path;
  d["content"] = b64enc(content);
  d["branch"] = cfg.ghb;
  if (sha.length()) d["sha"] = sha;
  String body;
  serializeJson(d, body);
  d.clear();
  HTTPClient h2;
  h2.setTimeout(20000);
  if (!h2.begin(c, url)) return false;
  ghHeaders(h2);
  h2.addHeader("Content-Type", "application/json");
  int code = h2.PUT(body);
  h2.end();
  return code == 200 || code == 201;
}
static bool ghUploadDatabase() {
  String csv = "employee_id,name,active\n";
  {
    Lock l(sdM);
    if (!sdOk) return false;
    for (auto& w : workers) csv += w.id + "," + w.name + "," + (w.active ? "1" : "0") + "\n";
  }
  if (!ghPut("config/workers.csv", csv)) return false;

  std::vector<String> files;
  {
    Lock l(sdM);
    if (!sdOk) return false;
    File dir = SD.open("/muhax/logs");
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return false;
    }
    File entry = dir.openNextFile();
    while (entry) {
      if (!entry.isDirectory()) {
        String name = entry.name();
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        if (name.endsWith(".csv")) files.push_back(name);
      }
      entry.close();
      entry = dir.openNextFile();
    }
    dir.close();
  }

  for (const String& name : files) {
    String content = readAll("/muhax/logs/" + name);
    if (content.length() == 0 || !ghPut("attendance/" + devId + "/" + name, content)) return false;
  }
  return true;
}
static void ghTask(void*) {
  for (;;) {
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    if (setupMode || maintenanceMode || WiFi.status() != WL_CONNECTED || !timeOk()) continue;
    String day;
    bool wk = false;
    bool database = false;
    uint32_t lc = 0;
    {
      Lock l(pendM);
      if (databaseUploadRequested) {
        database = true;
        databaseUploadRequested = false;
      } else if (millis() - lastChange >= (uint32_t)cfg.ghDelay * 1000UL) {
        if (npend > 0) day = pend[0];
        wk = workersDirty;
        lc = lastChange;
      }
    }
    if (!database && day == "" && !wk) continue;
    bool ok = false;
    if (database) {
      ok = ghUploadDatabase();
      if (!ok) {
        Lock l(pendM);
        databaseUploadRequested = true;
      }
    } else if (day != "") {
      String content = readAll("/muhax/logs/" + day + ".csv");
      ok = content.length() == 0 ? true : ghPut("attendance/" + devId + "/" + day + ".csv", content);
      if (ok) {
        Lock l(pendM);
        if (lastChange == lc && npend > 0 && pend[0] == day) {
          for (int i = 1; i < npend; i++) pend[i - 1] = pend[i];
          npend--;
        }
      }
    } else {
      String csv = "employee_id,name,active\n";
      {
        Lock l(sdM);
        for (auto& w : workers) csv += w.id + "," + w.name + "," + (w.active ? "1" : "0") + "\n";
      }
      ok = ghPut("config/workers.csv", csv);
      if (ok) workersDirty = false;
    }
    ghLastOk = ok;
    if (ok) lastGhOkMs = millis() ? millis() : 1;
    else vTaskDelay(30000 / portTICK_PERIOD_MS);  // back off, the SD card keeps everything
  }
}

// ---------------------------------------------------------------- display
#define COL_BG TFT_BLACK
#define COL_GREEN 0x0540
#define COL_RED 0xA800
static void setLed(bool r, bool g, bool b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);  // active low
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}
static void drawQR(const String& text, int x, int y, int scale, int version, int box) {
  static uint8_t qbuf[512];
  QRCode qr;
  tft.fillRect(x, y, box, box, TFT_WHITE);
  if (qrcode_initText(&qr, qbuf, version, ECC_LOW, text.c_str()) != 0) return;
  int px = qr.size * scale;
  int ox = x + (box - px) / 2, oy = y + (box - px) / 2;
  for (int j = 0; j < qr.size; j++)
    for (int i = 0; i < qr.size; i++)
      if (qrcode_getModule(&qr, i, j)) tft.fillRect(ox + i * scale, oy + j * scale, scale, scale, TFT_BLACK);
}
static String qrToken(long win) { return hmacHex(cfg.qrs, String(win)).substring(0, 8); }
static bool tokenValid(const String& t) {
  if (t.length() != 8 || !timeOk()) return false;
  long w = time(nullptr) / cfg.qrr;
  return t == qrToken(w) || t == qrToken(w - 1);
}
static String lastIp() { return WiFi.localIP().toString(); }

static void drawHeader() {
  tft.fillRect(0, 0, 320, 28, 0x2104);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, 0x2104);
  tft.drawString(cfg.company.length() ? cfg.company : String("MUHAX Logger"), 6, 14, 4);
  tft.setTextDatum(MR_DATUM);
  int x = 314;
  tft.setTextColor(ghLastOk ? TFT_GREEN : TFT_ORANGE, 0x2104); tft.drawString("GH", x, 14, 2); x -= 28;
  tft.setTextColor(sdOk ? TFT_GREEN : TFT_RED, 0x2104);        tft.drawString("SD", x, 14, 2); x -= 28;
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? TFT_GREEN : TFT_RED, 0x2104); tft.drawString("WiFi", x, 14, 2);
}
static void drawIdle(bool full) {
  time_t now = time(nullptr);
  if (full) {
    tft.fillScreen(COL_BG);
    drawHeader();
    lastWin = -1;
    lastSec = -1;
  }
  if (!timeOk()) {
    if (full) {
      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(TFT_ORANGE, COL_BG);
      tft.drawString("Getting time...", 160, 120, 4);
    }
    return;
  }
  long win = now / cfg.qrr;
  if (win != lastWin) {
    lastWin = win;
    String url = "http://" + lastIp() + "/q?t=" + qrToken(win);
    drawQR(url, 6, 40, 5, 4, 175);
  }
  int sec = (int)(now % 60);
  if (sec != lastSec || full) {
    lastSec = sec;
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, COL_BG);
    tft.setTextPadding(130);
    tft.drawString(fmtTime(now, "%H:%M:%S"), 188, 56, 4);
    tft.setTextPadding(130);
    tft.drawString(fmtTime(now, "%a %d %b"), 188, 90, 2);
    tft.drawString(fmtTime(now, "%Y"), 188, 108, 2);
    tft.setTextPadding(0);
    tft.setTextColor(TFT_CYAN, COL_BG);
    tft.drawString("Scan to", 188, 140, 2);
    tft.drawString("check in / out", 188, 158, 2);
    tft.setTextColor(TFT_LIGHTGREY, COL_BG);
    tft.setTextPadding(320);
    tft.setTextDatum(BL_DATUM);
    tft.drawString("In now: " + String(insideCount()) + "   " + lastLine, 6, 238, 2);
    tft.setTextPadding(0);
  }
}
static void drawNotice(const String& l1, const String& l2, uint16_t col) {
  tft.fillScreen(col);
  tft.setTextColor(TFT_WHITE, col);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(l1, 160, 100, 4);
  tft.drawString(l2, 160, 140, 2);
}
static void showResult(const Worker& w, int act, long workedSec) {
  uint16_t col = act == 1 ? COL_GREEN : COL_RED;
  tft.fillScreen(col);
  tft.setTextColor(TFT_WHITE, col);
  tft.setTextDatum(MC_DATUM);
  time_t now = time(nullptr);
  tft.drawString(act == 1 ? "Welcome!" : "Goodbye!", 160, 30, 4);
  tft.drawString(w.name.length() > 20 ? w.name.substring(0, 20) : w.name, 160, 85, 4);
  tft.drawString("ID: " + w.id, 160, 115, 2);
  tft.drawString(act == 1 ? "** CHECKED IN **" : "** CHECKED OUT **", 160, 152, 4);
  tft.drawString(fmtTime(now, "%H:%M"), 160, 188, 4);
  if (act == 2 && workedSec > 0) tft.drawString("Worked today: " + String(workedSec / 3600) + "h " + String((workedSec / 60) % 60) + "m", 160, 220, 2);
  else tft.drawString(fmtTime(now, "%A, %d %b %Y"), 160, 220, 2);
  setLed(act == 2, act == 1, false);
  ui = UI_RESULT;
  uiUntil = millis() + 3000;
  lastLine = "Last: " + w.name.substring(0, 12) + (act == 1 ? " IN " : " OUT ") + fmtTime(now, "%H:%M");
}
static void showInfo() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, COL_BG);
  int y = 6;
  auto line = [&](const String& s) { tft.drawString(s, 6, y, 2); y += 20; };
  line("INFO   " + cfg.company);
  line("WiFi: " + cfg.ssid + (WiFi.status() == WL_CONNECTED ? " (connected)" : " (offline)"));
  line("Site: http://" + lastIp());
  line(String("      http://") + MDNS_NAME + ".local");
  line("Time synced: " + (lastSyncMs ? String((millis() - lastSyncMs) / 60000) + " min ago" : String("never")));
  line(String("SD: ") + (sdOk ? "OK" : "MISSING") + "  workers: " + String(workers.size()));
  line(String("GitHub: ") + (lastGhOkMs ? String((millis() - lastGhOkMs) / 60000) + " min ago" : String("not yet")) + " (pending " + String(npend) + ")");
  line("Device: " + devId);
  tft.drawString("Join WiFi QR:", 6, y + 2, 2);
  drawQR("WIFI:T:WPA;S:" + cfg.ssid + ";P:" + cfg.wpass + ";;", 225, 130, 3, 3, 90);
  ui = UI_INFO;
  uiUntil = millis() + 10000;
}

// ---------------------------------------------------------------- web: common
static const char CSS[] PROGMEM =
    "*{box-sizing:border-box}body{font-family:system-ui,-apple-system,'Segoe UI',sans-serif;max-width:1120px;margin:auto;padding:26px 22px;background:#f4f7fb;color:#17233b;line-height:1.5}"
    "h1,h2,h3,p{margin-top:0}h1{font-size:clamp(26px,4vw,36px);line-height:1.15;letter-spacing:-.04em;margin-bottom:8px}h2{font-size:25px;letter-spacing:-.03em}h3{font-size:17px}"
    "a{color:#315ce8;text-decoration:none}a:hover{text-decoration:underline}header.topbar{display:flex;align-items:center;justify-content:space-between;gap:18px;padding:0 0 22px;border-bottom:1px solid #e2e8f2;margin-bottom:30px}"
    ".brand{display:flex;align-items:center;gap:10px;color:#17233b;font-weight:800;letter-spacing:.04em;white-space:nowrap}.brand:hover{text-decoration:none}.brand-icon{display:grid;place-items:center;width:34px;height:34px;border-radius:11px;background:#315ce8;color:#fff;font-size:17px}.brand small{font-size:10px;color:#8290a8;letter-spacing:.12em}"
    ".nav-links{display:flex;align-items:center;gap:6px;flex-wrap:wrap}.nav-links a{padding:8px 11px;border-radius:9px;color:#65728a;font-size:14px;font-weight:650}.nav-links a:hover{background:#e9eef8;color:#2449cd;text-decoration:none}"
    "input,select,textarea,button{width:100%;font:inherit;font-size:16px;padding:12px 14px;margin:5px 0;border-radius:10px;border:1px solid #d6deea;background:#fff;color:#17233b}"
    "input:focus,select:focus,textarea:focus{outline:3px solid #315ce822;border-color:#315ce8}label{display:block;font-size:13px;font-weight:700;color:#44516a;margin-top:12px}"
    "button{background:#315ce8;color:#fff;border:0;font-weight:750;cursor:pointer;transition:background .15s,transform .15s}button:hover{background:#2449cd;transform:translateY(-1px)}button.g{background:#16835d}button.g:hover{background:#106a4b}button.r{background:#c44747}button.r:hover{background:#a93434}"
    "button.s{width:auto;font-size:13px;padding:7px 10px;margin:2px;border-radius:8px}table{border-collapse:collapse;width:100%;font-size:14px;background:#fff;border-radius:12px;overflow:hidden}td,th{border-bottom:1px solid #e8edf4;padding:11px 12px;text-align:start}th{background:#f8faff;color:#69768d;font-size:12px;text-transform:uppercase;letter-spacing:.05em}tr:last-child td{border-bottom:0}"
    ".topbar{align-items:center}.big{font-size:28px;font-weight:750;text-align:center;padding:30px;border-radius:16px;color:#fff}.in{background:#16835d}.out{background:#c44747}.err{color:#b42336}"
    ".auth-shell{width:min(100%,440px);margin:7vh auto}.auth-card{padding:clamp(24px,6vw,40px);background:#fff;border:1px solid #e5eaf2;border-radius:22px;box-shadow:0 18px 55px #25365712}.auth-brand{margin-bottom:30px}.auth-kicker,.eyebrow{font-size:11px;font-weight:800;color:#315ce8;letter-spacing:.13em;text-transform:uppercase}.auth-copy,.muted{color:#78859b}.auth-copy{margin-bottom:26px}.login-form{margin-top:20px}.remember{display:flex;align-items:center;gap:9px;margin:14px 0;color:#647188;font-weight:500}.remember input{width:17px;height:17px;margin:0;accent-color:#315ce8}.auth-card button{margin-top:12px}.auth-foot{font-size:12px;text-align:center;color:#8a96aa;margin:22px 0 0}.alert{padding:11px 13px;border-radius:10px;background:#fff1f1;color:#ae2634;font-size:14px;font-weight:650;margin:14px 0}"
    ".dashboard{display:grid;gap:24px}.page-heading{display:flex;justify-content:space-between;align-items:flex-end;gap:20px}.page-heading p{color:#78859b;margin-bottom:0}.metrics{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px}.metric,.panel{background:#fff;border:1px solid #e5eaf2;border-radius:16px;padding:20px;box-shadow:0 5px 18px #25365708}.metric-label{display:block;color:#78859b;font-size:13px;font-weight:650}.metric strong{display:block;font-size:32px;line-height:1.2;letter-spacing:-.04em;margin:9px 0}.metric-note{font-size:12px;color:#8a96aa}.panel-head{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}.panel-head h2{font-size:18px;margin:0}.panel-head a{font-size:13px;font-weight:700}.status{display:inline-flex;align-items:center;gap:7px;border-radius:20px;padding:5px 9px;background:#e9f7f0;color:#14734f;font-size:12px;font-weight:750}.status:before{content:'';width:7px;height:7px;border-radius:50%;background:currentColor}.status.warn{background:#fff4df;color:#98630c}.people{list-style:none;padding:0;margin:0}.people li{display:flex;justify-content:space-between;gap:14px;padding:13px 0;border-bottom:1px solid #edf0f5}.people li:last-child{border-bottom:0}.person-name{font-weight:700}.person-meta{font-size:13px;color:#78859b}.empty{padding:22px 0;color:#78859b;text-align:center}.quick-links{display:flex;gap:10px;flex-wrap:wrap}.quick-links a{padding:9px 12px;border:1px solid #e0e6f0;border-radius:9px;background:#fff;font-size:13px;font-weight:700}"
    "@media(max-width:760px){body{padding:18px 14px}header.topbar{align-items:flex-start;flex-direction:column;gap:13px;margin-bottom:22px}.nav-links{width:100%;gap:3px}.nav-links a{padding:7px 9px;font-size:13px}.metrics{grid-template-columns:repeat(2,minmax(0,1fr))}.metric,.panel{padding:16px}.page-heading{align-items:flex-start;flex-direction:column}}"
    "@media(max-width:420px){.metrics{gap:9px}.metric{padding:13px}.metric strong{font-size:27px}.people li{flex-direction:column;gap:2px}td,th{padding:8px 7px;font-size:12px}}";
static String page(const String& title, const String& body) {
  return "<!doctype html><html dir=auto><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>" +
         esc(title) + "</title><style>" + String(FPSTR(CSS)) + "</style></head><body>" + body + "</body></html>";
}
static void sendPage(int code, const String& title, const String& body) {
  server.send(code, "text/html; charset=utf-8", page(title, body));
}
static String getCookie(const String& n) {
  String c = server.header("Cookie"), k = n + "=";
  int i = 0;
  while (true) {
    i = c.indexOf(k, i);
    if (i < 0) return "";
    if (i == 0 || c[i - 1] == ' ' || c[i - 1] == ';') {
      int e = c.indexOf(';', i);
      return c.substring(i + k.length(), e < 0 ? c.length() : e);
    }
    i += k.length();
  }
}
static void setCookie(const String& n, const String& v, long maxAge) {
  String s = n + "=" + v + "; Path=/; HttpOnly; SameSite=Lax";
  if (maxAge > 0) s += "; Max-Age=" + String(maxAge);
  server.sendHeader("Set-Cookie", s);
}
static void redirect(const String& to, int code = 303) {
  server.sendHeader("Location", to);
  server.send(code, "text/plain", "");
}

// ---------------------------------------------------------------- web: worker side
static bool passValid() {
  String c = getCookie("pp");
  int d = c.indexOf('.');
  if (d < 0) return false;
  long exp = c.substring(0, d).toInt();
  return exp > (long)time(nullptr) && c.substring(d + 1) == hmacHex(cfg.cks, "pp" + c.substring(0, d)).substring(0, 16);
}
static void issueRm(const Worker& w, const String& dev, bool remember) {
  long exp = (long)time(nullptr) + (remember ? 90L * 86400L : 12L * 3600L);
  String body = w.id + "." + String(w.gen) + "." + String(exp) + "." + dev;
  setCookie("rm", body + "." + hmacHex(cfg.cks, "rm" + body).substring(0, 20), remember ? 90L * 86400L : 0);
}
static Worker* whoAmI(String& dev) {
  String c = getCookie("rm");
  c.replace('.', ',');
  String p[5];
  if (splitCsv(c, p, 5) < 5) return nullptr;
  String body = p[0] + "." + p[1] + "." + p[2] + "." + p[3];
  if (p[4] != hmacHex(cfg.cks, "rm" + body).substring(0, 20)) return nullptr;
  if (p[2].toInt() < (long)time(nullptr)) return nullptr;
  Worker* w = findW(p[0]);
  if (!w || !w->active || w->gen != p[1].toInt()) return nullptr;
  dev = p[3];
  return w;
}
static int expectedAct(const Worker& w) {
  return (w.lastAct == 1 && sameDay(w.lastEpoch, time(nullptr))) ? 2 : 1;
}
static void scanAgain() {
  sendPage(200, "Scan again", "<h2>MUHAX Logger</h2><p class=err>Scan the code on the screen again.</p>");
}
static void resultPage(const Worker& w, int act, long worked) {
  String b = "<div class='big " + String(act == 1 ? "in" : "out") + "'>" + (act == 1 ? "CHECKED IN" : "CHECKED OUT") + "<br>" + esc(w.name) +
             "<br>" + fmtTime(time(nullptr), "%H:%M") + "</div>";
  if (act == 2 && worked > 0) b += "<p>Worked today: " + String(worked / 3600) + "h " + String((worked / 60) % 60) + "m</p>";
  sendPage(200, "Done", b);
}
static void hQ() {
  if (!timeOk()) { sendPage(503, "Starting", "<p>Starting up, try again in a few seconds.</p>"); return; }
  if (!tokenValid(server.arg("t"))) { scanAgain(); return; }
  long exp = (long)time(nullptr) + 180;
  setCookie("pp", String(exp) + "." + hmacHex(cfg.cks, "pp" + String(exp)).substring(0, 16), 180);
  redirect("/me", 302);
}
static void loginForm(const String& err) {
  String message = err.length() ? "<div class=alert role=alert>" + esc(err) + "</div>" : "";
  sendPage(200, "Employee sign in", "<main class=auth-shell><section class=auth-card><div class='brand auth-brand'><span class=brand-icon>M</span><span>MUHAX <small>LOGGER</small></span></div>"
                                    "<div class=auth-kicker>Employee portal</div><h1>Welcome back</h1><p class=auth-copy>Sign in to continue to your attendance.</p>" + message +
                                    "<form class=login-form method=post action=/login><label for=employee-id>Employee ID</label><input id=employee-id name=id placeholder='Enter your ID' autocomplete=username required>"
                                    "<label for=employee-pin>PIN</label><input id=employee-pin name=pin type=password inputmode=numeric pattern='[0-9]{4,6}' maxlength=6 placeholder='4-6 digit PIN' autocomplete=current-password required>"
                                    "<label class=remember><input type=checkbox name=rem value=1 checked>Remember this phone</label>"
                                    "<button type=submit>Sign in</button></form><p class=auth-foot>Secure check-in powered by MUHAX Logger</p></section></main>");
}
static void hMe() {
  if (!passValid()) { scanAgain(); return; }
  String dev;
  Worker* w = whoAmI(dev);
  if (!w) { loginForm(""); return; }
  int act = expectedAct(*w);
  String st = act == 2 ? "You are IN since " + fmtTime(w->lastEpoch, "%H:%M")
                       : (w->lastAct == 1 ? "Last IN was on " + fmtTime(w->lastEpoch, "%d %b") + " (no OUT recorded)" : "You are OUT");
  sendPage(200, "MUHAX", "<h2>Hello, " + esc(w->name) + "</h2><p>" + st + "</p><form method=post action=/punch><input type=hidden name=a value=" +
                             String(act) + "><button class='" + (act == 1 ? "g" : "r") + "'>" + (act == 1 ? "CHECK IN" : "CHECK OUT") + "</button></form>");
}
static void hLogin() {
  if (!passValid()) { scanAgain(); return; }
  Worker* w = findW(server.arg("id"));
  if (!w || !w->active) { delay(500); loginForm("Wrong ID or PIN"); return; }
  if (w->lockUntil && millis() < w->lockUntil) { loginForm("Too many tries, wait 5 minutes"); return; }
  if (sha256hex(w->salt + server.arg("pin")) != w->hash) {
    delay(500);
    if (++w->fails >= 5) { w->lockUntil = millis() + 300000UL; w->fails = 0; }
    loginForm("Wrong ID or PIN");
    return;
  }
  w->fails = 0;
  issueRm(*w, randHex(8), server.arg("rem") == "1");
  redirect("/me");
}
static void hPunch() {
  if (!passValid()) { scanAgain(); return; }
  String dev;
  Worker* w = whoAmI(dev);
  if (!w) { loginForm("Please log in"); return; }
  if (!timeOk()) { sendPage(503, "Starting", "<p>Starting up, try again.</p>"); return; }
  time_t now = time(nullptr);
  if (w->lastAct && (now - w->lastEpoch) < cfg.dbl) { resultPage(*w, w->lastAct, 0); return; }  // double tap
  if (!sdOk) { sendPage(500, "Error", "<p class=err>Storage error. Tell the admin.</p>"); return; }
  int act = expectedAct(*w);
  if (server.arg("a").toInt() != act) { redirect("/me"); return; }
  long worked = (act == 2) ? (long)(now - w->lastEpoch) : 0;
  if (w->lastAct == 1 && act == 1) appendLog(*w, "MISSING_OUT", dev);
  if (!appendLog(*w, act == 1 ? "IN" : "OUT", dev)) { sendPage(500, "Error", "<p class=err>Could not save. Tell the admin.</p>"); return; }
  w->lastAct = act;
  w->lastEpoch = now;
  saveWorkers();
  markDirty(fmtTime(now, "%Y-%m-%d"));
  showResult(*w, act, worked);
  resultPage(*w, act, worked);
}

// ---------------------------------------------------------------- web: admin
static bool adminOk() {
  String h = server.header("Authorization");
  if (cfg.au.length() && h.startsWith("Basic ")) {
    String d = b64dec(h.substring(6));
    int c = d.indexOf(':');
    if (c > 0 && d.substring(0, c) == cfg.au && sha256hex(cfg.asalt + d.substring(c + 1)) == cfg.ah) return true;
    delay(800);
  }
  server.requestAuthentication(BASIC_AUTH, "MUHAX Admin");
  return false;
}
#define NEED_ADMIN if (!adminOk()) return;
static String nav() {
  return "<header class=topbar><a class=brand href=/admin><span class=brand-icon>M</span><span>MUHAX <small>LOGGER</small></span></a>"
         "<nav class=nav-links><a href=/admin>Overview</a><a href=/admin/workers>Workers</a><a href=/admin/records>Records</a><a href=/admin/export>Export</a><a href=/admin/system>System</a></nav></header>";
}
static void aDash() {
  NEED_ADMIN
  time_t now = time(nullptr);
  int activeWorkers = 0;
  for (auto& w : workers) if (w.active) activeWorkers++;
  bool haveTime = timeOk();
  String company = cfg.company.length() ? esc(cfg.company) : String("Attendance overview");
  String syncStatus = ghLastOk ? "Synced" : "Waiting";
  String timeStatus = timeTrusted() ? "Trusted" : (haveTime ? "Needs sync" : "Not set");
  String b = nav() + "<main class=dashboard><section class=page-heading><div><div class=eyebrow>Overview</div><h1>" + company +
             "</h1><p>Live attendance and device health at a glance.</p></div><span class='status" + String(ghLastOk ? "" : " warn") + "'>" +
             syncStatus + "</span></section><section class=metrics><article class=metric><span class=metric-label>Currently checked in</span><strong>" +
             String(insideCount()) + "</strong><span class=metric-note>Workers on site today</span></article><article class=metric><span class=metric-label>Active workers</span><strong>" +
             String(activeWorkers) + "</strong><span class=metric-note>Enabled employee accounts</span></article><article class=metric><span class=metric-label>SD storage</span><strong>" +
             (sdOk ? String("Ready") : String("Missing")) + "</strong><span class=metric-note>" + (sdOk ? "Attendance logs available" : "Check-ins cannot be saved") +
             "</span></article><article class=metric><span class=metric-label>GitHub queue</span><strong>" + String(npend) +
             "</strong><span class=metric-note>Pending upload days</span></article></section><section class=panel><div class=panel-head><h2>Currently on site</h2><a href=/admin/records>View records</a></div><ul class=people>";
  int present = 0;
  for (auto& w : workers) {
    if (w.active && w.lastAct == 1 && sameDay(w.lastEpoch, now)) {
      b += "<li><span class=person-name>" + esc(w.name) + "<span class=person-meta> &middot; " + esc(w.id) +
           "</span></span><span class=person-meta>Checked in at " + fmtTime(w.lastEpoch, "%H:%M") + "</span></li>";
      present++;
    }
  }
  if (!present) b += "<li class=empty>No one is currently checked in.</li>";
  b += "</ul></section><section class=panel><div class=panel-head><h2>Device status</h2><span class='status" + String(timeTrusted() ? "" : " warn") +
       "'>" + timeStatus + "</span></div><p class=muted>" + (haveTime ? fmtTime(now, "%Y-%m-%d %H:%M:%S") : String("Device time has not been set yet.")) +
       "</p><div class=quick-links><a href=/admin/workers>Manage workers</a><a href=/admin/export>Export attendance</a><a href=/admin/system>System settings</a></div></section></main>";
  sendPage(200, "Dashboard", b);
}
static void aWorkers() {
  NEED_ADMIN
  String note;
  if (server.method() == HTTP_POST) {
    String op = server.arg("op"), id = server.arg("id");
    Worker* w = findW(id);
    if (op == "add") {
      String pin = server.arg("pin").length() ? server.arg("pin") : randPin();
      if (!validId(id) || w || !validPin(pin)) note = "<p class=err>Bad or duplicate ID, or PIN not 4-6 digits.</p>";
      else {
        Worker n; n.id = id; n.name = cleanField(server.arg("name")); setPin(n, pin);
        workers.push_back(n); saveWorkers();
        note = "<p>Added " + esc(id) + ". PIN (shown once): <b>" + pin + "</b></p>";
      }
    } else if (w) {
      if (op == "reset") { String pin = randPin(); setPin(*w, pin); w->gen++; saveWorkers(); note = "<p>New PIN for " + esc(id) + " (shown once): <b>" + pin + "</b></p>"; }
      else if (op == "unlink") { w->gen++; saveWorkers(); note = "<p>Phones unlinked.</p>"; }
      else if (op == "toggle") { w->active = !w->active; w->gen++; saveWorkers(); }
      else if (op == "delete") { for (size_t i = 0; i < workers.size(); i++) if (workers[i].id == id) { workers.erase(workers.begin() + i); break; } saveWorkers(); }
    }
  }
  String b = nav() + note + "<table><tr><th>ID<th>Name<th></tr>";
  for (auto& w : workers) {
    String f = "<form method=post style='display:inline'><input type=hidden name=id value='" + esc(w.id) + "'><input type=hidden name=op value=";
    b += "<tr><td>" + esc(w.id) + (w.active ? "" : " (off)") + "<td>" + esc(w.name) + "<td>" +
         f + "reset><button class=s>PIN</button></form>" + f + "unlink><button class=s>Unlink</button></form>" +
         f + "toggle><button class=s>" + (w.active ? "Disable" : "Enable") + "</button></form>" +
         f + "delete><button class='s r' onclick=\"return confirm('Delete?')\">Del</button></form></tr>";
  }
  b += "</table><h3>Add worker</h3><form method=post><input type=hidden name=op value=add><input name=id placeholder='ID (letters/digits)' required>"
       "<input name=name placeholder=Name required><input name=pin placeholder='PIN 4-6 digits (blank = random)' inputmode=numeric><button>Add</button></form>";
  sendPage(200, "Workers", b);
}
static void aRecords() {
  NEED_ADMIN
  String day = server.hasArg("date") ? server.arg("date") : fmtTime(time(nullptr), "%Y-%m-%d");
  String b = nav() + "<form><input type=date name=date value='" + esc(day) + "'><button>Show</button></form><table>";
  String csv = readAll("/muhax/logs/" + cleanField(day) + ".csv");
  int st = 0;
  bool first = true;
  while (st < (int)csv.length()) {
    int e = csv.indexOf('\n', st);
    if (e < 0) e = csv.length();
    String p[9];
    if (splitCsv(csv.substring(st, e), p, 9) >= 5) {
      b += "<tr>";
      String t = first ? "th" : "td";
      b += "<" + t + ">" + esc(p[0]) + "<" + t + ">" + esc(p[2]) + "<" + t + ">" + esc(p[3]) + "<" + t + ">" + esc(p[4]) + (first || p[7] == "1" ? "" : " (time?)") + "</tr>";
      first = false;
    }
    st = e + 1;
  }
  sendPage(200, "Records", b + "</table>");
}
static void aExport() {
  NEED_ADMIN
  if (!server.hasArg("from")) {
    String d = fmtTime(time(nullptr), "%Y-%m-%d");
    sendPage(200, "Export", nav() + "<form><label>From</label><input type=date name=from value=" + d + "><label>To</label><input type=date name=to value=" + d +
                                "><input name=id placeholder='Employee ID (optional)'><button>Download CSV</button></form>");
    return;
  }
  struct tm a = {}, z = {};
  if (sscanf(server.arg("from").c_str(), "%d-%d-%d", &a.tm_year, &a.tm_mon, &a.tm_mday) != 3 ||
      sscanf(server.arg("to").c_str(), "%d-%d-%d", &z.tm_year, &z.tm_mon, &z.tm_mday) != 3) { server.send(400, "text/plain", "bad date"); return; }
  a.tm_year -= 1900; a.tm_mon -= 1; a.tm_hour = 12; z.tm_year -= 1900; z.tm_mon -= 1; z.tm_hour = 12;
  time_t t0 = mktime(&a), t1 = mktime(&z);
  String only = server.arg("id");
  server.sendHeader("Content-Disposition", "attachment; filename=attendance_" + server.arg("from") + "_" + server.arg("to") + ".csv");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv; charset=utf-8", "");
  server.sendContent("\xEF\xBB\xBF");  // BOM so Excel reads Arabic
  server.sendContent("timestamp_local,epoch_utc,employee_id,name,action,method,phone_id,time_trusted,device_id\n");
  int days = 0;
  for (time_t t = t0; t <= t1 && days < 400; t += 86400, days++) {
    String csv = readAll("/muhax/logs/" + fmtTime(t, "%Y-%m-%d") + ".csv");
    int st = 0;
    while (st < (int)csv.length()) {
      int e = csv.indexOf('\n', st);
      if (e < 0) e = csv.length();
      String line = csv.substring(st, e);
      st = e + 1;
      if (!line.length() || line.startsWith("timestamp")) continue;
      String p[4];
      splitCsv(line, p, 4);
      if (only.length() && p[2] != only) continue;
      server.sendContent(line + "\n");
    }
  }
  server.sendContent("");
}
static void aSystem() {
  NEED_ADMIN
  String note;
  int responseCode = 200;
  if (server.method() == HTTP_POST) {
    String op = server.arg("op");
    if (op == "sync") { requestDatabaseUpload(); note = "<p>Full database upload queued.</p>"; }
    else if (op == "timesync") { resyncNow = true; note = "<p>Time sync requested.</p>"; }
    else if (op == "settime") {
      long e = server.arg("epoch").toInt();
      if (e > 1700000000) { struct timeval tv = {e, 0}; settimeofday(&tv, nullptr); lastSyncMs = millis() ? millis() : 1; lastWin = -1; needFull = true; note = "<p>Time set from your phone.</p>"; }
    } else if (op == "restart") { restartAt = millis() + 1500; note = "<p>Restarting...</p>"; }
    else if (op == "setup") { prefs.begin("mx", false); prefs.putBool("force", true); prefs.end(); restartAt = millis() + 1500; note = "<p>Going to setup mode...</p>"; }
    else if (op == "format") {
      if (server.arg("confirm") != "FORMAT") {
        responseCode = 400;
        note = "<p class=err>Type FORMAT exactly to confirm the SD card erase.</p>";
      } else {
        String error;
        if (formatSdCard(error)) {
          restartAt = millis() + 1500;
          note = "<p>SD card formatted. All card data has been erased; the device is restarting.</p>";
        } else {
          responseCode = 500;
          note = "<p class=err>" + esc(error) + "</p>";
        }
      }
    } else if (op == "factory") {
      if (server.arg("confirm") != "RESET") {
        responseCode = 400;
        note = "<p class=err>Type RESET exactly to confirm the factory reset.</p>";
      } else {
        String error;
        if (factoryReset(error)) {
          note = "<p>Factory reset complete. Settings, workers, and attendance logs were erased. The device is restarting in setup mode.</p>";
        } else {
          responseCode = 500;
          note = "<p class=err>" + esc(error) + "</p>";
        }
      }
    }
  }
  auto f = [](const char* op, const char* label, const char* cls = "") { return String("<form method=post><input type=hidden name=op value=") + op + "><button class='" + cls + "'>" + label + "</button></form>"; };
  String b = nav() + note + f("sync", "Upload database to GitHub now") + f("timesync", "Sync time now") +
             "<form method=post><input type=hidden name=op value=settime><input type=hidden name=epoch id=e><button onclick=\"document.getElementById('e').value=Math.floor(Date.now()/1000)\">Set time from my phone</button></form>" +
             f("restart", "Restart") + f("setup", "Go to setup mode (settings page)", "r") +
             "<section class=panel><h2>Format SD card</h2><p>This formats the entire card and permanently erases all files, including files outside the logger.</p>"
             "<form method=post onsubmit=\"return confirm('Format the entire SD card and erase every file?')\"><input type=hidden name=op value=format>"
             "<label for=format-confirm>Type FORMAT to confirm</label><input id=format-confirm name=confirm autocomplete=off required>"
             "<button class=r>Format SD card</button></form></section>"
             "<section class=panel><h2>Factory reset</h2><p>Erases saved device settings, the worker roster, and attendance logs. Other files on the SD card are kept.</p>"
             "<form method=post onsubmit=\"return confirm('Erase device settings, workers, and attendance logs?')\"><input type=hidden name=op value=factory>"
             "<label for=reset-confirm>Type RESET to confirm</label><input id=reset-confirm name=confirm autocomplete=off required>"
             "<button class=r>Factory reset</button></form></section>";
  sendPage(responseCode, "System", b);
}

// ---------------------------------------------------------------- web: setup
static void sRoot() {
  int n = WiFi.scanNetworks();
  String opts;
  for (int i = 0; i < n && i < 20; i++) opts += "<option value='" + esc(WiFi.SSID(i)) + "'>";
  bool have = configured();
  String b = "<h2>MUHAX Logger setup</h2><form id=setup-form method=post action=/save>"
             "<h3>Admin account</h3><input name=au placeholder='Admin username' value='" + esc(cfg.au) + "'><input name=ap type=password placeholder='Admin password (min 6)'>"
             "<h3>Company</h3><input name=co placeholder='Company name' value='" + esc(cfg.company) + "'>"
             "<h3>Company WiFi (2.4 GHz)</h3><input name=ssid list=nets placeholder=SSID value='" + esc(cfg.ssid) + "'><datalist id=nets>" + opts +
             "</datalist><input name=wp type=password placeholder='WiFi password'><button type=submit formaction=/test/wifi formtarget=test-result>Test WiFi</button>"
             "<h3>Time</h3><input name=tz type=number value=" + String(cfg.tzh) + "> hours from UTC (Jordan = 3)<input name=sh type=number value=" + String(cfg.syncH) + "> re-sync every N hours"
             "<h3>GitHub (private repo)</h3><input name=gho placeholder='Owner' value='" + esc(cfg.gho) + "'><input name=ghr placeholder='Repo name' value='" + esc(cfg.ghr) +
             "'><input name=ghb placeholder=Branch value='" + esc(cfg.ghb) + "'><input name=ght type=password placeholder='Fine-grained token (Contents: read/write)'><button type=submit formaction=/test/github formtarget=test-result>Test GitHub</button>"
             "<h3>Workers</h3><textarea name=w rows=5 placeholder='One per line: id,name,pin&#10;pin may be blank (random)'></textarea>"
             "<h3>Options</h3><input name=qrr type=number value=" + String(cfg.qrr) + "> QR rotation seconds<input name=dbl type=number value=" + String(cfg.dbl) + "> double-tap seconds"
             + (have ? "<p>Blank passwords/token keep the saved values.</p>" : "") +
             "<button>SAVE AND RESTART</button></form><h3>Test result</h3>"
             "<iframe name=test-result title='WiFi or GitHub test result' style='width:100%;height:90px;border:1px solid #d6deea;border-radius:10px' "
             "srcdoc='<!doctype html><html><body style=\"font:16px sans-serif;color:#647188\">Run a test to see its result.</body></html>'></iframe>";
  sendPage(200, "Setup", b);
}
static void sendTestResult(bool ok, const String& message) {
  String color = ok ? "#16835d" : "#b42336";
  server.send(200, "text/html; charset=utf-8",
              "<!doctype html><html><meta name=viewport content='width=device-width,initial-scale=1'><body style='font:16px system-ui,sans-serif;color:" +
                  color + ";margin:14px'><strong>" + String(ok ? "SUCCESS" : "FAILED") + "</strong><br>" + esc(message) + "</body></html>");
}
static bool validGithubSegment(const String& value) {
  if (!value.length() || value == "." || value == "..") return false;
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (!isAlphaNumeric(c) && c != '-' && c != '_' && c != '.') return false;
  }
  return true;
}
static void sTestWifi() {
  String ssid = server.arg("ssid");
  String password = server.arg("wp");
  if (!ssid.length()) { sendTestResult(false, "Enter a WiFi name first."); return; }
  if (!password.length() && ssid == cfg.ssid) password = cfg.wpass;

  WiFi.disconnect(false);
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str());
  uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000UL) delay(250);
  bool connected = WiFi.status() == WL_CONNECTED;
  String result = connected ? "WiFi connected successfully. IP: " + WiFi.localIP().toString()
                            : "Could not connect to that WiFi. Check the name, password, and 2.4 GHz availability.";

  WiFi.disconnect(false);
  delay(100);
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());
  sendTestResult(connected, result);
}
static void sTestGithub() {
  String owner = cleanField(server.arg("gho"));
  String repo = cleanField(server.arg("ghr"));
  String token = server.arg("ght");
  if (!token.length()) token = cfg.ght;
  if (!validGithubSegment(owner) || !validGithubSegment(repo)) {
    sendTestResult(false, "Enter a valid GitHub owner and repository name.");
    return;
  }
  if (!token.length()) { sendTestResult(false, "Enter a GitHub token first."); return; }

  WiFiClientSecure client;
#if GITHUB_INSECURE_TLS
  client.setInsecure();
#endif
  client.setTimeout(15);
  HTTPClient http;
  http.setTimeout(15000);
  String url = "https://api.github.com/repos/" + owner + "/" + repo;
  if (!http.begin(client, url)) {
    sendTestResult(false, "Could not start the GitHub request.");
    return;
  }
  ghHeaders(http, token);
  int code = http.GET();
  http.end();
  if (code == 200) sendTestResult(true, "GitHub connection and repository access OK.");
  else if (code == 401) sendTestResult(false, "GitHub rejected the token (HTTP 401).");
  else if (code == 404) sendTestResult(false, "Repository not found or token has no access (HTTP 404).");
  else if (code < 0) sendTestResult(false, "GitHub request failed: " + HTTPClient::errorToString(code));
  else sendTestResult(false, "GitHub check failed with HTTP " + String(code) + ".");
}
static void sSave() {
  String au = server.arg("au"), ap = server.arg("ap");
  if (au.length()) cfg.au = cleanField(au);
  if (ap.length()) {
    if (ap.length() < 6) { sendPage(400, "Error", "<p class=err>Admin password must be at least 6 characters.</p><a href=/>Back</a>"); return; }
    cfg.asalt = randHex(8);
    cfg.ah = sha256hex(cfg.asalt + ap);
  }
  if (!cfg.au.length() || !cfg.ah.length() || !server.arg("ssid").length()) {
    sendPage(400, "Error", "<p class=err>Admin username, admin password and WiFi name are required.</p><a href=/>Back</a>");
    return;
  }
  cfg.company = cleanField(server.arg("co"));
  if (server.arg("wp").length() || server.arg("ssid") != cfg.ssid) cfg.wpass = server.arg("wp");
  cfg.ssid = server.arg("ssid");
  cfg.tzh = constrain(server.arg("tz").toInt(), -12, 14);
  cfg.syncH = max(1, (int)server.arg("sh").toInt());
  cfg.gho = cleanField(server.arg("gho"));
  cfg.ghr = cleanField(server.arg("ghr"));
  cfg.ghb = server.arg("ghb").length() ? cleanField(server.arg("ghb")) : String("main");
  if (server.arg("ght").length()) cfg.ght = server.arg("ght");
  cfg.qrr = constrain(server.arg("qrr").toInt(), 10, 300);
  cfg.dbl = constrain(server.arg("dbl").toInt(), 1, 60);
  if (!cfg.qrs.length()) cfg.qrs = randHex(32);
  if (!cfg.cks.length()) cfg.cks = randHex(32);
  saveCfg();
  prefs.begin("mx", false); prefs.putBool("force", false); prefs.end();

  String pins;
  if (sdOk) {
    loadWorkers();
    String txt = server.arg("w");
    int st = 0;
    while (st < (int)txt.length()) {
      int e = txt.indexOf('\n', st);
      if (e < 0) e = txt.length();
      String p[3];
      String line = txt.substring(st, e);
      st = e + 1;
      line.trim();
      if (!line.length()) continue;
      int c = splitCsv(line, p, 3);
      String id = cleanField(p[0]), name = c > 1 ? cleanField(p[1]) : id, pin = c > 2 ? cleanField(p[2]) : "";
      if (!validId(id)) continue;
      if (!validPin(pin)) pin = randPin();
      Worker* w = findW(id);
      if (!w) { Worker n; n.id = id; workers.push_back(n); w = &workers.back(); }
      w->name = name;
      setPin(*w, pin);
      pins += "<tr><td>" + esc(id) + "<td>" + esc(name) + "<td><b>" + pin + "</b></tr>";
    }
    saveWorkers();
  }
  sendPage(200, "Saved", "<h2>Saved</h2><p>The device restarts and joins the company WiFi. Its address will be on the screen.</p>" +
                             (pins.length() ? "<p>PINs (shown once, write them down):</p><table>" + pins + "</table>" : String("")) +
                             (sdOk ? "" : "<p class=err>No SD card: workers were not saved.</p>"));
  restartAt = millis() + 4000;
}
static void sNotFound() { redirect("http://192.168.4.1/", 302); }

// ---------------------------------------------------------------- modes
static void routes() {
  const char* keys[] = {"Cookie", "Authorization"};
  server.collectHeaders(keys, 2);
  if (setupMode) {
    server.on("/", HTTP_GET, sRoot);
    server.on("/test/wifi", HTTP_POST, sTestWifi);
    server.on("/test/github", HTTP_POST, sTestGithub);
    server.on("/save", HTTP_POST, sSave);
    server.on("/admin", HTTP_GET, []() { redirect("/", 302); });
    server.onNotFound(sNotFound);
  } else {
    server.on("/", HTTP_GET, []() { redirect("/me", 302); });
    server.on("/q", HTTP_GET, hQ);
    server.on("/me", HTTP_GET, hMe);
    server.on("/login", HTTP_POST, hLogin);
    server.on("/punch", HTTP_POST, hPunch);
    server.on("/admin", HTTP_GET, aDash);
    server.on("/admin/workers", aWorkers);
    server.on("/admin/records", HTTP_GET, aRecords);
    server.on("/admin/export", HTTP_GET, aExport);
    server.on("/admin/system", aSystem);
    server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  }
  server.begin();
}
static void drawSetupScreen() {
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_YELLOW, COL_BG);
  tft.drawString("FIRST-TIME SETUP", 6, 6, 4);
  tft.setTextColor(TFT_WHITE, COL_BG);
  tft.drawString("1. Join WiFi:", 6, 50, 2);
  tft.drawString(AP_SSID, 20, 70, 4);
  tft.drawString("Password:", 20, 104, 2);
  tft.drawString(apPass, 20, 122, 4);
  tft.drawString("2. Open in browser:", 6, 170, 2);
  tft.drawString("http://192.168.4.1", 20, 190, 4);
  drawQR("WIFI:T:WPA;S:" + String(AP_SSID) + ";P:" + apPass + ";;", 225, 50, 3, 3, 90);
}
static void startSetup() {
  setupMode = true;
  const char* al = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
  apPass = "";
  for (int i = 0; i < 8; i++) apPass += al[esp_random() % 32];
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, apPass.c_str());
  if (cfg.ssid.length()) WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());  // recovery: keep trying the saved network
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
  routes();
  drawSetupScreen();
  setLed(false, false, true);
}
static void startRun() {
  setupMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(MDNS_NAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.wpass.c_str());
  drawNotice("Connecting WiFi...", cfg.ssid, 0x2104);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  sntp_set_time_sync_notification_cb(onSync);
  startNtp();
  MDNS.begin(MDNS_NAME);
  routes();
  drawNotice("Ready", "http://" + lastIp(), 0x2104);
  delay(6000);  // show the address for a few seconds after start-up
  needFull = true;
}

// ---------------------------------------------------------------- arduino
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  setLed(false, false, false);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  tft.init();
  tft.setRotation(1);  // landscape 320x240
  tft.fillScreen(COL_BG);
  sdM = xSemaphoreCreateMutex();
  pendM = xSemaphoreCreateMutex();
  uint64_t m = ESP.getEfuseMac();
  char id[16];
  sprintf(id, "mx-%06x", (unsigned)((m >> 24) & 0xFFFFFF));
  devId = id;

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOk = SD.begin(SD_CS, sdSPI) && SD.cardType() != CARD_NONE;
  if (sdOk) { SD.mkdir("/muhax"); SD.mkdir("/muhax/logs"); }
  loadCfg();
  prefs.begin("mx", true);
  bool force = prefs.getBool("force", false);
  prefs.end();
  if (sdOk) loadWorkers();
  if (!configured() || force) startSetup();
  else startRun();
  xTaskCreatePinnedToCore(ghTask, "gh", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  server.handleClient();
  if (setupMode) {
    dns.processNextRequest();
    static uint32_t chk = 0;
    if (millis() - chk > 30000) {  // saved WiFi is back and nobody is configuring: go back to normal
      chk = millis();
      if (configured() && WiFi.status() == WL_CONNECTED && WiFi.softAPgetStationNum() == 0) {
        prefs.begin("mx", false); prefs.putBool("force", false); prefs.end();
        ESP.restart();
      }
    }
  } else {
    // WiFi lost for 2 minutes -> open the setup network again so settings can be fixed
    if (WiFi.status() != WL_CONNECTED) {
      if (!wifiLostSince) wifiLostSince = millis();
      else if (millis() - wifiLostSince > 120000UL) {  // restart into setup mode; it returns to normal by itself when the WiFi is back
        prefs.begin("mx", false); prefs.putBool("force", true); prefs.end();
        ESP.restart();
      }
    } else wifiLostSince = 0;

    if (WiFi.status() == WL_CONNECTED && (resyncNow || millis() - lastResync > (uint32_t)cfg.syncH * 3600000UL ||
                                           (!timeOk() && millis() - lastResync > 30000UL))) {
      resyncNow = false;
      startNtp();
    }
    // ui state machine
    if (ui != UI_IDLE && millis() > uiUntil) { ui = UI_IDLE; setLed(false, false, false); needFull = true; }
    if (ui == UI_IDLE) {
      drawIdle(needFull);
      needFull = false;
    }
  }
  if (restartAt && millis() > restartAt) ESP.restart();

  // BOOT button: short press = info, hold 10 s = setup mode
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!btnDown) btnDown = millis();
    if (millis() - btnDown > 10000UL) {
      prefs.begin("mx", false); prefs.putBool("force", true); prefs.end();
      drawNotice("Setup mode", "Release the button", 0x4208);
      delay(1500);
      ESP.restart();
    }
  } else if (btnDown) {
    if (millis() - btnDown > 50 && millis() - btnDown < 2000 && !setupMode) showInfo();
    btnDown = 0;
  }
  delay(5);
}
