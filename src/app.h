#pragma once
#include <Arduino.h>

// Hooks implemented in main.cpp and used by web.cpp.
void app_requestRestart(uint32_t delayMs = 1500);
void app_enterSetupMode();               // forced setup mode (keeps workers and records)
extern String g_apPass;                  // current setup-network password
extern int g_tapsToday;
