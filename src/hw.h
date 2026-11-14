#pragma once
#include <Arduino.h>

namespace Nfc {
bool begin();                       // false if the PN532 does not answer
bool ready();
bool poll(String& uidHex);          // non-blocking-ish (~30 ms); true when a card was read
void retryInit();                   // call periodically when !ready()
extern volatile uint32_t tapSeq;    // increments on every card read (for "tap card now")
extern String lastUid;
}

namespace Led {
void begin();
void set(bool r, bool g, bool b);   // board LED is active low
void off();
}

namespace Touch {
void begin();
bool pressed();                     // true while the screen is touched
}

namespace Boot {
bool heldFor(uint32_t ms);          // BOOT button continuously low for ms
}
