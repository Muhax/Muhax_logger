#pragma once
#include <Arduino.h>

struct Settings {
  bool configured = false;
  String company = "";
  String wifiSsid = "", wifiPass = "";
  String adminUser = "admin", adminSalt = "", adminHash = "";
  int tzMinutes = 180;      // Jordan UTC+3
  int syncHours = 2;
  String ghOwner = "", ghRepo = "", ghBranch = "main", ghToken = "";
  int dupSeconds = 5;
  bool led = true;
  bool dim = true;
  int uploadDelaySec = 90;
  String deviceId = "";

  void load();
  void save();
  void factoryReset();
  bool githubConfigured() const { return ghOwner.length() && ghRepo.length() && ghToken.length(); }
  void setAdminPassword(const String& pw);
  bool checkAdmin(const String& user, const String& pw) const;
};

extern Settings cfg;

String randomSetupPassword();          // 8 unambiguous chars
String hashPassword(const String& salt, const String& pw);
String hexOf(const uint8_t* d, size_t n);
