#include "hw.h"
#include "config.h"
#include "settings.h"
#include <Wire.h>
#include <Adafruit_PN532.h>

// ------------------------------------------------------------------ NFC
namespace Nfc {

#ifdef PN532_UART
static Adafruit_PN532 pn(255, &Serial2);   // UART (HSU) module: GND VCC TXD RXD
#else
static Adafruit_PN532 pn(-1, -1, &Wire);   // I2C, no IRQ / RESET pins used
#endif
static bool gReady = false;
static uint32_t lastTry = 0;
volatile uint32_t tapSeq = 0;
String lastUid;

bool begin() {
#ifdef PN532_UART
  pn.begin();                  // the library re-opens Serial2 on default pins...
  Serial2.end();               // ...so re-open it on the connector pins afterwards
  Serial2.begin(115200, SERIAL_8N1, PIN_NFC_RX, PIN_NFC_TX);
  delay(50);
  pn.wakeup();
#else
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);
  pn.begin();
#endif
  uint32_t v = pn.getFirmwareVersion();
  if (!v) { gReady = false; return false; }
  pn.SAMConfig();
  pn.setPassiveActivationRetries(0x10);
  gReady = true;
  return true;
}

bool ready() { return gReady; }

void retryInit() {
  if (gReady || millis() - lastTry < 5000) return;
  lastTry = millis();
  begin();
}

bool poll(String& uidHex) {
  if (!gReady) return false;
  uint8_t uid[10];
  uint8_t len = 0;
  bool ok = pn.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 40);
  if (!ok || len == 0) return false;
  uidHex = hexOf(uid, len);
  lastUid = uidHex;
  tapSeq = tapSeq + 1;
  return true;
}

}  // namespace Nfc

// ------------------------------------------------------------------ LED
namespace Led {
void begin() {
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_B, OUTPUT);
  off();
}
void set(bool r, bool g, bool b) {
  if (!cfg.led) { off(); return; }
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}
void off() {
  digitalWrite(PIN_LED_R, HIGH);
  digitalWrite(PIN_LED_G, HIGH);
  digitalWrite(PIN_LED_B, HIGH);
}
}  // namespace Led

// ------------------------------------------------------------------ Touch (XPT2046)
// Only "is the screen touched" is needed, so the controller is read with a tiny
// bit-banged SPI (own pins, no bus shared with the TFT or SD card).
namespace Touch {

static uint16_t xptRead(uint8_t cmd) {
  digitalWrite(PIN_TOUCH_CS, LOW);
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_TOUCH_MOSI, (cmd >> i) & 1);
    digitalWrite(PIN_TOUCH_CLK, HIGH);
    digitalWrite(PIN_TOUCH_CLK, LOW);
  }
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(PIN_TOUCH_CLK, HIGH);
    digitalWrite(PIN_TOUCH_CLK, LOW);
    v = (v << 1) | digitalRead(PIN_TOUCH_MISO);
  }
  digitalWrite(PIN_TOUCH_CS, HIGH);
  return v >> 3;
}

void begin() {
  pinMode(PIN_TOUCH_CS, OUTPUT);
  pinMode(PIN_TOUCH_CLK, OUTPUT);
  pinMode(PIN_TOUCH_MOSI, OUTPUT);
  pinMode(PIN_TOUCH_MISO, INPUT);
  pinMode(PIN_TOUCH_IRQ, INPUT);
  digitalWrite(PIN_TOUCH_CS, HIGH);
  digitalWrite(PIN_TOUCH_CLK, LOW);
  xptRead(0x90);    // arm the pen-interrupt output
}

bool pressed() {
  if (digitalRead(PIN_TOUCH_IRQ) != LOW) return false;
  xptRead(0xB1);    // pressure read: keeps the controller awake with PENIRQ enabled
  return digitalRead(PIN_TOUCH_IRQ) == LOW;
}

}  // namespace Touch

// ------------------------------------------------------------------ BOOT button
namespace Boot {
static uint32_t downSince = 0;
bool heldFor(uint32_t ms) {
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!downSince) downSince = millis();
    return millis() - downSince >= ms;
  }
  downSince = 0;
  return false;
}
}  // namespace Boot
