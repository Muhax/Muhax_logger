// I2C scanner for the PN532. Tries both pin orders because the connector labels are unclear.
// The PN532 answers at address 0x24.
#include <Arduino.h>
#include <Wire.h>

static int scan(int sda, int scl) {
  Wire.end();
  Wire.begin(sda, scl);
  Wire.setClock(100000);
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X%s\n", a, a == 0x24 ? "  <-- PN532" : "");
      found++;
    }
  }
  return found;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== I2C scan, both pin orders ===");
}

void loop() {
  Serial.println("Order A: SDA=GPIO27, SCL=GPIO22");
  int a = scan(27, 22);
  Serial.println("Order B: SDA=GPIO22, SCL=GPIO27");
  int b = scan(22, 27);
  if (!a && !b) Serial.println("NO I2C DEVICE in either order: check power (VCC to 3V3), GND, and the module's I2C switches.");
  delay(1500);
}
