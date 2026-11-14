#include "settings.h"
#include <Preferences.h>
#include <WiFi.h>
#include <mbedtls/md.h>
#include <esp_system.h>
#include <esp_mac.h>

Settings cfg;

String hexOf(const uint8_t* d, size_t n) {
  static const char* H = "0123456789ABCDEF";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
  return s;
}

static void sha256(const uint8_t* in, size_t len, uint8_t out[32]) {
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), in, len, out);
}

// Salted, iterated SHA-256. The password itself is never stored.
String hashPassword(const String& salt, const String& pw) {
  String seed = salt + ":" + pw;
  uint8_t h[32];
  sha256((const uint8_t*)seed.c_str(), seed.length(), h);
  for (int i = 0; i < 2000; i++) {
    uint8_t buf[32 + 16];
    memcpy(buf, h, 32);
    size_t sl = min((size_t)16, (size_t)salt.length());
    memcpy(buf + 32, salt.c_str(), sl);
    sha256(buf, 32 + sl, h);
  }
  return hexOf(h, 32);
}

String randomSetupPassword() {
  static const char* A = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no 0/O/1/I
  String s;
  for (int i = 0; i < 8; i++) s += A[esp_random() % 32];
  return s;
}

void Settings::load() {
  Preferences p;
  p.begin("muhax", true);
  configured = p.getBool("cfg", false);
  company = p.getString("company", "");
  wifiSsid = p.getString("ssid", "");
  wifiPass = p.getString("wpass", "");
  adminUser = p.getString("auser", "admin");
  adminSalt = p.getString("asalt", "");
  adminHash = p.getString("ahash", "");
  tzMinutes = p.getInt("tz", 180);
  syncHours = p.getInt("synch", 2);
  ghOwner = p.getString("gho", "");
  ghRepo = p.getString("ghr", "");
  ghBranch = p.getString("ghb", "main");
  ghToken = p.getString("ght", "");
  dupSeconds = p.getInt("dup", 5);
  led = p.getBool("led", true);
  dim = p.getBool("dim", true);
  uploadDelaySec = p.getInt("updelay", 90);
  p.end();
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  deviceId = "muhax-" + hexOf(mac + 3, 3);
  deviceId.toLowerCase();
  if (syncHours < 1) syncHours = 1;
  if (dupSeconds < 1) dupSeconds = 1;
}

void Settings::save() {
  Preferences p;
  p.begin("muhax", false);
  p.putBool("cfg", configured);
  p.putString("company", company);
  p.putString("ssid", wifiSsid);
  p.putString("wpass", wifiPass);
  p.putString("auser", adminUser);
  p.putString("asalt", adminSalt);
  p.putString("ahash", adminHash);
  p.putInt("tz", tzMinutes);
  p.putInt("synch", syncHours);
  p.putString("gho", ghOwner);
  p.putString("ghr", ghRepo);
  p.putString("ghb", ghBranch);
  p.putString("ght", ghToken);
  p.putInt("dup", dupSeconds);
  p.putBool("led", led);
  p.putBool("dim", dim);
  p.putInt("updelay", uploadDelaySec);
  p.end();
}

void Settings::factoryReset() {
  Preferences p;
  p.begin("muhax", false);
  p.clear();
  p.end();
}

void Settings::setAdminPassword(const String& pw) {
  adminSalt = randomSetupPassword() + randomSetupPassword();
  adminHash = hashPassword(adminSalt, pw);
}

bool Settings::checkAdmin(const String& user, const String& pw) const {
  if (!configured || adminHash.length() == 0) return false;
  if (user != adminUser) return false;
  return hashPassword(adminSalt, pw) == adminHash;
}
