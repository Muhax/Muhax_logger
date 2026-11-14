#pragma once
#include <Arduino.h>
#include <time.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct Worker {
  String id, uid, name;
  bool active = true;
  // runtime state (rebuilt from /muhax/state.log and today's log)
  bool in = false;
  time_t lastEpoch = 0;          // 0 = unknown/pending
  uint32_t workedToday = 0;      // seconds
  String workedDay;
};

struct Record {
  time_t epoch = 0;
  uint32_t bootSec = 0;          // for taps made before the first time sync
  uint32_t bootId = 0;
  String uid, empId, name;
  bool in = false;
  bool trusted = false;
  bool corrected = false;
  bool pending = false;
  String flag;                   // e.g. MISSING_OUT_PREV, TIME_UNKNOWN
};

struct UnknownCard {
  String uid;
  time_t last = 0;
  uint16_t count = 0;
};

// Recursive lock shared by the main loop and the GitHub upload task (one SPI bus).
SemaphoreHandle_t sdMutex();
struct SdLock {
  SdLock();
  ~SdLock();
};

namespace Storage {
extern std::vector<Worker> workers;
extern std::vector<UnknownCard> unknown;
extern volatile uint32_t lastWriteMs;

bool begin();
void loop();                         // remount attempts, queue flush
bool ok();
bool loadWorkers();
bool saveWorkers();
Worker* byUid(const String& uid);
Worker* byId(const String& id);
bool validId(const String& id);
bool validUid(const String& uid);

String csvField(const String& s);
void splitCsv(const String& line, std::vector<String>& out);

bool commit(Record& r);              // tap -> pending / SD / flash queue (false = could not be stored)
void resolvePending();               // after the first time sync
void loadTodayTotals();
int insideCount();
int pendingCount();
int queuedCount();
uint32_t bootId();

void addUnknown(const String& uid);

String logPath(const String& day);
std::vector<String> listDays();                           // sorted "YYYY-MM-DD"
size_t readChunk(const String& path, size_t offset, String& out, size_t maxBytes);
bool readFile(const String& path, String& out, size_t maxBytes);
bool writeFile(const String& path, const String& data);   // atomic-ish (tmp + rename)
bool appendText(const String& path, const String& text);
size_t fileSize(const String& path);
}  // namespace Storage
