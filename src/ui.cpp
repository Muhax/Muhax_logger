#include "ui.h"
#include "config.h"
#include <TFT_eSPI.h>
#include <qrcode.h>

static TFT_eSPI tft;

#define C_BG      TFT_BLACK
#define C_IN      tft.color565(0, 110, 40)
#define C_OUT     tft.color565(170, 30, 30)
#define C_WARN    tft.color565(220, 110, 0)
#define C_DIM     tft.color565(130, 130, 130)
#define C_ACCENT  tft.color565(80, 200, 255)

static IdleData prev;
static bool prevValid = false;

namespace Ui {

String ascii(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c >= 32 && c < 127) o += (char)c;
    else if (c < 0x80) continue;
    else if ((c & 0xC0) != 0x80) o += '?';   // first byte of a multi-byte char -> one '?'
  }
  return o;
}

void setBacklight(uint8_t level) { ledcWrite(0, level); }

void begin() {
  tft.init();
  tft.setRotation(1);   // landscape 320x240
  tft.fillScreen(C_BG);
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_BACKLIGHT, 0);
  setBacklight(255);
}

static void center(const String& s, int y, int font, uint16_t fg, uint16_t bg) {
  tft.setTextColor(fg, bg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextPadding(0);
  tft.drawString(ascii(s), 160, y, font);
}

// Fit a name into the width using font 4, else font 2.
static void fitText(const String& s, int y, uint16_t fg, uint16_t bg) {
  String t = ascii(s);
  int font = tft.textWidth(t, 4) <= 300 ? 4 : 2;
  center(t, y, font, fg, bg);
}

void splash(const String& title, const String& sub) {
  prevValid = false;
  tft.fillScreen(C_BG);
  center("MUHAX Logger", 80, 4, TFT_WHITE, C_BG);
  center(title, 130, 4, C_ACCENT, C_BG);
  if (sub.length()) center(sub, 165, 2, C_DIM, C_BG);
}

void setupScreen(const String& ssid, const String& pass, const String& ip) {
  prevValid = false;
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString("MUHAX Logger - SETUP", 8, 6, 2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("1. Join WiFi:", 8, 34, 2);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString(ascii(ssid), 8, 52, 4);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Password:", 8, 84, 2);
  tft.setTextColor(TFT_YELLOW, C_BG);
  tft.drawString(pass, 8, 102, 4);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("2. Open in browser:", 8, 140, 2);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString("http://" + ip, 8, 158, 2);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("Waiting for setup...", 8, 214, 2);

  // QR code that joins the WiFi network
  String payload = "WIFI:T:WPA;S:" + ssid + ";P:" + pass + ";;";
  QRCode qr;
  uint8_t buf[qrcode_getBufferSize(4)];
  qrcode_initText(&qr, buf, 4, ECC_LOW, payload.c_str());
  const int scale = 3, quiet = 3;
  int size = (qr.size + quiet * 2) * scale;
  int x0 = 320 - size - 6, y0 = 34;
  tft.fillRect(x0, y0, size, size, TFT_WHITE);
  for (int y = 0; y < qr.size; y++)
    for (int x = 0; x < qr.size; x++)
      if (qrcode_getModule(&qr, x, y))
        tft.fillRect(x0 + (x + quiet) * scale, y0 + (y + quiet) * scale, scale, scale, TFT_BLACK);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("scan to join", x0 + size / 2, y0 + size + 4, 1);
}

void idleFull() {
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(TFT_WHITE, C_BG);
  tft.drawString("MUHAX Logger", 8, 6, 2);
  tft.drawFastHLine(0, 26, 320, tft.color565(50, 50, 50));
  center("Tap your card", 168, 4, C_ACCENT, C_BG);
  prevValid = false;
}

static void drawBadge(int x, const char* label, uint16_t col) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(TFT_BLACK, col);
  tft.fillRoundRect(x, 5, 40, 16, 3, col);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(label, x + 20, 13, 1);
}

void idleTick(const IdleData& d) {
  if (!prevValid || d.wifi != prev.wifi || d.sd != prev.sd || d.cloud != prev.cloud) {
    drawBadge(160, d.wifi ? "WiFi" : "OFF", d.wifi ? TFT_GREEN : C_WARN);
    drawBadge(206, d.sd ? "SD" : "NO SD", d.sd ? TFT_GREEN : TFT_RED);
    uint16_t cc = d.cloud == 1 ? TFT_GREEN : (d.cloud == 2 ? C_WARN : C_DIM);
    drawBadge(262, d.cloud == 0 ? "-" : (d.cloud == 1 ? "Cloud" : "Cloud!"), cc);
  }
  if (!prevValid || d.timeOk != prev.timeOk || d.clock != prev.clock) {
    tft.setTextPadding(300);
    tft.setTextDatum(MC_DATUM);
    if (d.timeOk) {
      tft.setTextColor(TFT_WHITE, C_BG);
      tft.drawString(d.clock, 160, 72, 6);
    } else {
      tft.setTextColor(C_WARN, C_BG);
      tft.drawString("Getting time...", 160, 72, 4);
    }
  }
  if (!prevValid || d.timeOk != prev.timeOk || d.date != prev.date) {
    tft.setTextPadding(310);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(d.timeOk ? d.date : "", 160, 124, 4);
  }
  if (!prevValid || d.inNow != prev.inNow || d.last != prev.last) {
    tft.setTextPadding(0);
    tft.fillRect(0, 205, 320, 35, C_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString("In now: " + String(d.inNow), 8, 215, 2);
    if (d.last.length()) {
      tft.setTextDatum(TR_DATUM);
      tft.drawString("Last: " + ascii(d.last), 312, 215, 2);
    }
  }
  prev = d;
  prevValid = true;
}

void welcome(bool in, const String& name, const String& id, const String& hhmm, const String& date, const String& worked) {
  prevValid = false;
  uint16_t bg = in ? C_IN : C_OUT;
  tft.fillScreen(bg);
  center(in ? "Welcome!" : "Goodbye!", 28, 4, TFT_WHITE, bg);
  fitText(name, 78, TFT_WHITE, bg);
  center("ID: " + id, 112, 4, TFT_WHITE, bg);
  center(in ? "** CHECKED IN **" : "** CHECKED OUT **", 152, 4, TFT_YELLOW, bg);
  center(hhmm, 192, 6, TFT_WHITE, bg);
  String bottom = in ? date : (worked.length() ? "Worked today: " + worked : date);
  center(bottom, 226, 2, TFT_WHITE, bg);
}

void warning(const String& title, const String& l1, const String& l2, const String& l3) {
  prevValid = false;
  tft.fillScreen(C_WARN);
  center(title, 50, 4, TFT_WHITE, C_WARN);
  if (l1.length()) center(l1, 110, 4, TFT_WHITE, C_WARN);
  if (l2.length()) center(l2, 150, 2, TFT_WHITE, C_WARN);
  if (l3.length()) center(l3, 180, 2, TFT_WHITE, C_WARN);
}

void info(const InfoData& d) {
  prevValid = false;
  tft.fillScreen(C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.drawString("INFO", 8, 6, 4);
  int y = 36;
  auto row = [&](const char* k, const String& v) {
    tft.setTextColor(C_DIM, C_BG);
    tft.drawString(k, 8, y, 2);
    tft.setTextColor(TFT_WHITE, C_BG);
    tft.drawString(ascii(v), 82, y, 2);
    y += 20;
  };
  row("Company", d.company.length() ? d.company : "-");
  row("WiFi", d.wifi);
  row("Admin", d.adminIp);
  if (d.adminMdns.length()) row("", d.adminMdns);
  row("Time", d.time);
  row("SD card", d.sd);
  row("GitHub", d.github);
  if (d.extra.length()) row("Note", d.extra);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(BR_DATUM);
  tft.drawString("[ touch to go back ]", 312, 236, 2);
}

}  // namespace Ui
