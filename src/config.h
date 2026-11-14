#pragma once
#include <Arduino.h>

// ---- CYD pin map (ESP32-2432S028R) ----
#define PIN_I2C_SDA   27   // PN532 SDA (side connector CN1)
#define PIN_I2C_SCL   22   // PN532 SCL
// PN532 over UART (define PN532_UART in platformio.ini): module TXD -> PIN_NFC_RX, module RXD -> PIN_NFC_TX
// Default: GPIO35 (RX, input-only pin, fine for receiving) and GPIO22 (TX), both on the CYD side connectors.
#define PIN_NFC_RX    35
#define PIN_NFC_TX    22
#define PIN_SD_CS     5
#define PIN_SD_SCK    18
#define PIN_SD_MISO   19
#define PIN_SD_MOSI   23
#define PIN_LED_R     4    // active low
#define PIN_LED_G     16
#define PIN_LED_B     17
#define PIN_BACKLIGHT 21
#define PIN_BOOT      0
#define PIN_TOUCH_CLK 25
#define PIN_TOUCH_MOSI 32
#define PIN_TOUCH_MISO 39
#define PIN_TOUCH_CS  33
#define PIN_TOUCH_IRQ 36

// ---- Setup network ----
#define SETUP_SSID    "Muhax_logger"
#define SETUP_IP      192, 168, 4, 1       // decision 4: 192.168.4.1
#define SETUP_IP_STR  "192.168.4.1"
#define HOSTNAME      "muhax-logger"

// ---- Behaviour ----
#define FW_VERSION          "1.0.0"
#define WIFI_LOST_RECOVERY_MS   (120UL * 1000UL)   // WiFi down this long -> setup network returns
#define BOOT_HOLD_SETUP_MS      (10UL * 1000UL)
#define TIME_UNTRUSTED_AFTER_S  (24UL * 3600UL)
#define RESULT_SCREEN_MS        3000UL
#define STARTUP_INFO_MS         10000UL
#define MAX_UNKNOWN_CARDS       10
#define FLASH_QUEUE_MAX_BYTES   3000
