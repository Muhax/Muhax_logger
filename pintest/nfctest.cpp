// PN532 UART pin test (NOT the real firmware). The PN532 sits on the CYD serial connector (GPIO3 = RX, GPIO1 = TX),
// so the USB serial port is unusable for output. Results are shown on the RGB LED instead:
//   RED blinking       : no answer from the PN532 (check HSU mode switches, power, wires)
//   GREEN solid        : PN532 answered, wiring correct (CYD RXD <- module TXD, CYD TXD -> module RXD)
//   YELLOW solid       : PN532 answered only with RX/TX swapped (wires crossed the wrong way round)
//   BLUE/WHITE flash   : a card was seen on the reader (flash colour = white when wiring is correct)
#include <Arduino.h>

#define LED_R 4    // active low
#define LED_G 16
#define LED_B 17

static void led(bool r, bool g, bool b) {
  digitalWrite(LED_R, r ? LOW : HIGH);
  digitalWrite(LED_G, g ? LOW : HIGH);
  digitalWrite(LED_B, b ? LOW : HIGH);
}

// Sends a frame, collects the answer for ms milliseconds.
static int exchange(const uint8_t* tx, int txn, uint8_t* rx, int rxmax, int ms) {
  while (Serial2.available()) Serial2.read();
  Serial2.write(tx, txn);
  uint32_t t0 = millis();
  int n = 0;
  while (millis() - t0 < (uint32_t)ms && n < rxmax) {
    if (Serial2.available()) rx[n++] = Serial2.read();
  }
  return n;
}

// Wakes the module and asks for its firmware version. True if a valid PN532 reply came back.
static bool probe(int rxPin, int txPin) {
  Serial2.end();
  Serial2.begin(115200, SERIAL_8N1, rxPin, txPin);
  delay(30);
  uint8_t rx[64];
  const uint8_t wake[] = {0x55, 0x55, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  Serial2.write(wake, sizeof(wake));
  delay(20);
  const uint8_t fw[] = {0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, 0x02, 0x2A, 0x00};
  int n = exchange(fw, sizeof(fw), rx, sizeof(rx), 300);
  for (int i = 0; i + 4 < n; i++)
    if (rx[i] == 0xD5 && rx[i + 1] == 0x03) return true;
  return false;
}

static bool cardSeen() {
  uint8_t rx[64];
  const uint8_t sam[] = {0x00, 0x00, 0xFF, 0x05, 0xFB, 0xD4, 0x14, 0x01, 0x14, 0x01, 0x02, 0x00};
  exchange(sam, sizeof(sam), rx, sizeof(rx), 150);
  const uint8_t poll[] = {0x00, 0x00, 0xFF, 0x04, 0xFC, 0xD4, 0x4A, 0x01, 0x00, 0xE1, 0x00};
  int m = exchange(poll, sizeof(poll), rx, sizeof(rx), 600);
  return m > 12;
}

void setup() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  led(false, false, false);
}

void loop() {
  // Order A is the intended wiring; order B has RX and TX swapped.
  if (probe(3, 1)) {
    led(false, true, false);
    if (cardSeen()) { led(true, true, true); delay(300); led(false, true, false); }
    delay(100);
    return;
  }
  if (probe(1, 3)) {
    led(true, true, false);
    if (cardSeen()) { led(false, false, true); delay(300); led(true, true, false); }
    delay(100);
    return;
  }
  led(true, false, false);
  delay(250);
  led(false, false, false);
  delay(250);
}
