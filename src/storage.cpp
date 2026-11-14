#include "storage.h"
#include "config.h"
#include "clock.h"
#include "settings.h"
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <algorithm>

static SemaphoreHandle_t gMutex = nullptr;
SemaphoreHandle_t sdMutex() {
  if (!gMutex) gMutex = xSemaphoreCreateRecursiveMutex();
  return gMutex;
}
SdLock::SdLock() { xSemaphoreTakeRecursive(sdMutex(), portMAX_DELAY); }
SdLock::~SdLock() { xSemaphoreGiveRecursive(sdMutex()); }

namespace Storage {

std::vector<Worker> workers;
std::vector<UnknownCard> unknown;
volatile uint32_t lastWriteMs = 0;

static const char* DIR_ROOT = "/muhax";
static const char* DIR_LOGS = "/muhax/logs";
static const char* F_WORKERS = "/muhax/workers.csv";
static const char* F_STATE = "/muhax/state.log";
static const char* F_PENDING = "/muhax/logs/pending-time.csv";
static const char* LOG_HEADER =
    "timestamp_local,epoch_utc,uid,employee_id,name,action,time_trusted,time_corrected,device_id,flag\n";

static bool sdOk = false;
static uint32_t gBootId = 0;
static uint32_t lastCheck = 0;
static std::vector<Record> pendingRam;

uint32_t bootId() { return gBootId; }
bool ok() { return sdOk; }

// ---------------------------------------------------------------- helpers
String csvField(const String& s) {
  bool q = false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == ',' || c == '"' || c == '\n' || c == '\r') { q = true; break; }
  }
  if (!q) return s;
  String o = "\"";
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '"') o += "\"\"";
    else if (s[i] == '\r' || s[i] == '\n') o += ' ';
    else o += s[i];
  }
  o += "\"";
  return o;
}

void splitCsv(const String& line, std::vector<String>& out) {
  out.clear();
  String cur;
  bool inq = false;
  for (size_t i = 0; i < line.length(); i++) {
    char c = line[i];
    if (inq) {
      if (c == '"') {
        if (i + 1 < line.length() && line[i + 1] == '"') { cur += '"'; i++; }
        else inq = false;
      } else cur += c;
    } else {
      if (c == '"') inq = true;
      else if (c == ',') { out.push_back(cur); cur = ""; }
      else if (c != '\r' && c != '\n') cur += c;
    }
  }
  out.push_back(cur);
}

bool validId(const String& id) {
  if (id.length() < 1 || id.length() > 16) return false;
  for (size_t i = 0; i < id.length(); i++) {
    char c = id[i];
    if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) return false;
  }
  return true;
}

bool validUid(const String& uid) {
  if (uid.length() < 8 || uid.length() > 20 || (uid.length() & 1)) return false;
  for (size_t i = 0; i < uid.length(); i++)
    if (!isxdigit((unsigned char)uid[i])) return false;
  return true;
}

Worker* byUid(const String& uid) {
  for (auto& w : workers) if (w.uid.equalsIgnoreCase(uid)) return &w;
  return nullptr;
}
Worker* byId(const String& id) {
  for (auto& w : workers) if (w.id == id) return &w;
  return nullptr;
}

// ---------------------------------------------------------------- files
bool appendText(const String& path, const String& text) {
  if (!sdOk) return false;
  SdLock l;
  File f = SD.open(path, FILE_APPEND);
  if (!f) { sdOk = false; return false; }
  size_t n = f.print(text);
  f.close();
  if (n != text.length()) { sdOk = false; return false; }
  return true;
}

size_t fileSize(const String& path) {
  if (!sdOk) return 0;
  SdLock l;
  File f = SD.open(path, FILE_READ);
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

bool readFile(const String& path, String& out, size_t maxBytes) {
  out = "";
  if (!sdOk) return false;
  SdLock l;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  if (f.size() > maxBytes) { f.close(); return false; }
  out.reserve(f.size());
  while (f.available()) {
    uint8_t buf[256];
    int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.concat((const char*)buf, n);
  }
  f.close();
  return true;
}

bool writeFile(const String& path, const String& data) {
  if (!sdOk) return false;
  SdLock l;
  String tmp = path + ".tmp";
  File f = SD.open(tmp, FILE_WRITE);
  if (!f) return false;
  size_t n = f.print(data);
  f.close();
  if (n != data.length()) { SD.remove(tmp); return false; }
  SD.remove(path);
  return SD.rename(tmp, path);
}

// Reads whole lines starting at offset, up to maxBytes. Returns the new offset
// (== offset when finished / on error).
size_t readChunk(const String& path, size_t offset, String& out, size_t maxBytes) {
  out = "";
  if (!sdOk) return offset;
  SdLock l;
  File f = SD.open(path, FILE_READ);
  if (!f) return offset;
  size_t sz = f.size();
  if (offset >= sz) { f.close(); return offset; }
  f.seek(offset);
  size_t pos = offset;
  while (f.available() && out.length() < maxBytes) {
    String line = f.readStringUntil('\n');
    pos += line.length() + 1;
    out += line;
    out += '\n';
  }
  f.close();
  return pos > sz ? sz : pos;
}

String logPath(const String& day) { return String(DIR_LOGS) + "/" + day + ".csv"; }

std::vector<String> listDays() {
  std::vector<String> v;
  if (!sdOk) return v;
  SdLock l;
  File dir = SD.open(DIR_LOGS);
  if (!dir) return v;
  File f;
  while ((f = dir.openNextFile())) {
    String n = f.name();
    f.close();
    int s = n.lastIndexOf('/');
    if (s >= 0) n = n.substring(s + 1);
    if (n.length() == 14 && n.endsWith(".csv")) {
      String d = n.substring(0, 10);
      if (Clock::isValidDay(d)) v.push_back(d);
    }
  }
  dir.close();
  std::sort(v.begin(), v.end());
  return v;
}

// ---------------------------------------------------------------- workers
bool loadWorkers() {
  workers.clear();
  if (!sdOk) return false;
  String txt;
  if (!readFile(F_WORKERS, txt, 64 * 1024)) {
    // interrupted save? use the temp file
    if (!readFile(String(F_WORKERS) + ".tmp", txt, 64 * 1024)) return true;   // no workers yet
  }
  int start = 0;
  bool first = true;
  while (start < (int)txt.length()) {
    int nl = txt.indexOf('\n', start);
    if (nl < 0) nl = txt.length();
    String line = txt.substring(start, nl);
    start = nl + 1;
    if (line.length() == 0) continue;
    if (first) { first = false; if (line.startsWith("employee_id")) continue; }
    std::vector<String> f;
    splitCsv(line, f);
    if (f.size() < 3 || !validId(f[0])) continue;
    Worker w;
    w.id = f[0];
    w.uid = f[1];
    w.uid.toUpperCase();
    w.name = f[2];
    w.active = f.size() < 4 || f[3] != "0";
    workers.push_back(w);
  }
  return true;
}

bool saveWorkers() {
  String s = "employee_id,uid,name,active\n";
  for (auto& w : workers) {
    s += csvField(w.id) + "," + w.uid + "," + csvField(w.name) + "," + (w.active ? "1" : "0") + "\n";
  }
  return writeFile(F_WORKERS, s);
}

// ---------------------------------------------------------------- state log
// Append-only "empId,I|O,epoch" lines; the last line per worker wins.
static void applyState() {
  String txt;
  if (!readFile(F_STATE, txt, 128 * 1024)) return;
  int start = 0;
  while (start < (int)txt.length()) {
    int nl = txt.indexOf('\n', start);
    if (nl < 0) nl = txt.length();
    String line = txt.substring(start, nl);
    start = nl + 1;
    std::vector<String> f;
    splitCsv(line, f);
    if (f.size() < 3) continue;
    Worker* w = byId(f[0]);
    if (!w) continue;
    w->in = f[1] == "I";
    w->lastEpoch = (time_t)f[2].toInt();
  }
  if (txt.length() > 32 * 1024) {      // compact
    String c;
    for (auto& w : workers)
      if (w.lastEpoch) c += w.id + (w.in ? ",I," : ",O,") + String((long)w.lastEpoch) + "\n";
    writeFile(F_STATE, c);
  }
}

static void appendState(const String& empId, bool in, time_t epoch) {
  appendText(F_STATE, empId + (in ? ",I," : ",O,") + String((long)epoch) + "\n");
}

// ---------------------------------------------------------------- flash queue (SD missing)
static String qRead() {
  Preferences p;
  p.begin("muhaxq", true);
  String s = p.getString("q", "");
  p.end();
  return s;
}
static void qWrite(const String& s) {
  Preferences p;
  p.begin("muhaxq", false);
  if (s.length()) p.putString("q", s); else p.remove("q");
  p.end();
}
int queuedCount() {
  String q = qRead();
  int n = 0;
  for (size_t i = 0; i < q.length(); i++) if (q[i] == '\n') n++;
  return n;
}
static bool queueLine(const String& line, const String& empId, bool in, time_t epoch) {
  String q = qRead();
  String add = line + "\x1f" + empId + (in ? "\x1fI\x1f" : "\x1fO\x1f") + String((long)epoch) + "\n";
  if (q.length() + add.length() > FLASH_QUEUE_MAX_BYTES) return false;
  qWrite(q + add);
  return true;
}
static void flushQueue() {
  String q = qRead();
  if (q.length() == 0) return;
  int start = 0;
  while (start < (int)q.length()) {
    int nl = q.indexOf('\n', start);
    if (nl < 0) nl = q.length();
    String item = q.substring(start, nl);
    start = nl + 1;
    int a = item.indexOf('\x1f');
    int b = item.indexOf('\x1f', a + 1);
    int c = item.indexOf('\x1f', b + 1);
    if (a < 0 || b < 0 || c < 0) continue;
    String line = item.substring(0, a);
    String emp = item.substring(a + 1, b);
    bool in = item.substring(b + 1, c) == "I";
    time_t ep = (time_t)item.substring(c + 1).toInt();
    String day = line.substring(0, 10);
    String path = logPath(day);
    if (!SD.exists(path)) appendText(path, LOG_HEADER);
    if (!appendText(path, line + "\n")) return;      // SD failed again, keep the rest
    appendState(emp, in, ep);
    lastWriteMs = millis();
  }
  qWrite("");
}

// ---------------------------------------------------------------- records
static String buildLine(const Record& r) {
  String ts = Clock::fmt(r.epoch, "%Y-%m-%d %H:%M:%S");
  return ts + "," + String((long)r.epoch) + "," + r.uid + "," + csvField(r.empId) + "," + csvField(r.name) + "," +
         (r.in ? "IN" : "OUT") + "," + (r.trusted ? "1" : "0") + "," + (r.corrected ? "1" : "0") + "," +
         cfg.deviceId + "," + r.flag;
}

static void updateWorkerFromRecord(Worker& w, const Record& r) {
  String d = Clock::day(r.epoch);
  if (w.workedDay != d) { w.workedDay = d; w.workedToday = 0; }
  if (!r.in && w.in && w.lastEpoch > 0 && Clock::day(w.lastEpoch) == d && r.epoch >= w.lastEpoch)
    w.workedToday += (uint32_t)(r.epoch - w.lastEpoch);
  w.in = r.in;
  w.lastEpoch = r.epoch;
}

static bool writeFinal(Record& r) {
  String line = buildLine(r);
  String path = logPath(Clock::day(r.epoch));
  bool done = false;
  if (sdOk) {
    SdLock l;
    if (!SD.exists(path)) appendText(path, LOG_HEADER);
    done = appendText(path, line + "\n");
    if (done) appendState(r.empId, r.in, r.epoch);
  }
  if (!done) done = queueLine(line, r.empId, r.in, r.epoch);   // SD problem: keep it in flash
  lastWriteMs = millis();
  return done;
}

// Returns false only if the record could not be stored anywhere.
bool commit(Record& r) {
  Worker* w = byId(r.empId);
  if (r.pending) {
    r.bootId = gBootId;
    r.bootSec = Clock::bootSeconds();
    pendingRam.push_back(r);
    appendText(F_PENDING, String(gBootId) + "," + String(r.bootSec) + "," + r.uid + "," + csvField(r.empId) + "," +
                              csvField(r.name) + "," + (r.in ? "IN" : "OUT") + "\n");
    if (w) { w->in = r.in; w->lastEpoch = 0; }
    return true;
  }
  if (w) updateWorkerFromRecord(*w, r);
  return writeFinal(r);
}

static void loadPendingFile() {
  String txt;
  if (!sdOk || !readFile(F_PENDING, txt, 32 * 1024)) return;
  int start = 0;
  while (start < (int)txt.length()) {
    int nl = txt.indexOf('\n', start);
    if (nl < 0) nl = txt.length();
    String line = txt.substring(start, nl);
    start = nl + 1;
    std::vector<String> f;
    splitCsv(line, f);
    if (f.size() < 6) continue;
    Record r;
    r.bootId = (uint32_t)f[0].toInt();
    r.bootSec = (uint32_t)f[1].toInt();
    r.uid = f[2];
    r.empId = f[3];
    r.name = f[4];
    r.in = f[5] == "IN";
    r.pending = true;
    pendingRam.push_back(r);
  }
}

int pendingCount() { return pendingRam.size(); }

void resolvePending() {
  if (!Clock::synced() || pendingRam.empty()) return;
  time_t now = Clock::now();
  for (auto& r : pendingRam) {
    if (r.bootId == gBootId) {
      uint32_t ago = Clock::bootSeconds() - r.bootSec;
      r.epoch = now - ago;
      r.trusted = true;
      r.corrected = true;
    } else {                       // from an earlier power-up: real time is unknown
      r.epoch = now;
      r.trusted = false;
      r.corrected = false;
      r.flag = "TIME_UNKNOWN";
    }
    r.pending = false;
    Worker* w = byId(r.empId);
    if (w) {
      // keep the worker's state coherent with the newest resolved record
      if (r.epoch >= w->lastEpoch) updateWorkerFromRecord(*w, r);
    }
    writeFinal(r);
  }
  pendingRam.clear();
  if (sdOk) { SdLock l; SD.remove(F_PENDING); }
}

void loadTodayTotals() {
  if (!Clock::synced() || !sdOk) return;
  time_t now = Clock::now();
  String d = Clock::day(now);
  size_t off = 0;
  std::vector<String> inAtUid;
  struct P { String id; time_t in; };
  std::vector<P> open;
  for (auto& w : workers) { w.workedToday = 0; w.workedDay = d; }
  String chunk;
  String path = logPath(d);
  while (true) {
    size_t n = readChunk(path, off, chunk, 2048);
    if (n == off) break;
    off = n;
    int s = 0;
    while (s < (int)chunk.length()) {
      int nl = chunk.indexOf('\n', s);
      if (nl < 0) nl = chunk.length();
      String line = chunk.substring(s, nl);
      s = nl + 1;
      std::vector<String> f;
      splitCsv(line, f);
      if (f.size() < 6 || f[0] == "timestamp_local") continue;
      time_t ep = (time_t)f[1].toInt();
      String id = f[3];
      bool in = f[5] == "IN";
      Worker* w = byId(id);
      if (!w) continue;
      if (in) {
        w->in = true;
        w->lastEpoch = ep;
        bool found = false;
        for (auto& p : open) if (p.id == id) { p.in = ep; found = true; }
        if (!found) open.push_back({id, ep});
      } else {
        for (auto it = open.begin(); it != open.end(); ++it)
          if (it->id == id) { if (ep >= it->in) w->workedToday += ep - it->in; open.erase(it); break; }
        w->in = false;
        w->lastEpoch = ep;
      }
    }
  }
}

int insideCount() {
  int n = 0;
  String today = Clock::synced() ? Clock::day(Clock::now()) : "";
  for (auto& w : workers) {
    if (!w.in || !w.active) continue;
    if (!Clock::synced() || w.lastEpoch == 0 || Clock::day(w.lastEpoch) == today) n++;
  }
  return n;
}

void addUnknown(const String& uid) {
  time_t now = Clock::now();
  for (auto it = unknown.begin(); it != unknown.end(); ++it) {
    if (it->uid == uid) {
      UnknownCard c = *it;
      c.count++;
      c.last = now;
      unknown.erase(it);
      unknown.insert(unknown.begin(), c);
      return;
    }
  }
  UnknownCard c;
  c.uid = uid;
  c.last = now;
  c.count = 1;
  unknown.insert(unknown.begin(), c);
  if (unknown.size() > MAX_UNKNOWN_CARDS) unknown.pop_back();
}

// ---------------------------------------------------------------- mount
static bool mount() {
  SdLock l;
  SD.end();
  SPI.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  if (!SD.begin(PIN_SD_CS, SPI, 16000000)) return false;
  if (SD.cardType() == CARD_NONE) return false;
  if (!SD.exists(DIR_ROOT)) SD.mkdir(DIR_ROOT);
  if (!SD.exists(DIR_LOGS)) SD.mkdir(DIR_LOGS);
  return SD.exists(DIR_LOGS);
}

static void afterMount() {
  loadWorkers();
  applyState();
  loadPendingFile();
  flushQueue();
}

bool begin() {
  sdMutex();
  Preferences p;
  p.begin("muhax", false);
  gBootId = p.getUInt("boot", 0) + 1;
  p.putUInt("boot", gBootId);
  p.end();
  sdOk = mount();
  if (sdOk) afterMount();
  return sdOk;
}

void loop() {
  if (millis() - lastCheck < 5000) return;
  lastCheck = millis();
  if (sdOk) {
    SdLock l;
    if (!SD.exists(DIR_LOGS)) sdOk = false;     // card removed
  }
  if (!sdOk) {
    if (mount()) { sdOk = true; afterMount(); }
  }
}

}  // namespace Storage
