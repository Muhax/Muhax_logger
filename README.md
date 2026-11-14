# MUHAX Logger (firmware)

NFC check-in/out terminal for the **ESP32-2432S028R "CYD"** + **PN532** (UART on GPIO 35 RX / GPIO 22 TX). Design: see [muhax-logger.md](muhax-logger.md).

## Build and flash

```
pio run -e cyd                 # build
pio run -e cyd -t upload       # flash (hold BOOT while plugging in if it does not connect)
pio device monitor             # serial log, 115200
```

Display variants (the CYD has several revisions): try `cyd` first (ILI9341). If the screen is blank or colours are
inverted use `cyd_ili9341_inv`; for the 2-USB-port revision use `cyd_st7789`. They differ only in the TFT_eSPI flags in
[platformio.ini](platformio.ini) (TFT_eSPI is configured there, no library files need editing).

Wiring: PN532 in **HSU (UART) mode** (SEL0 = OFF, SEL1 = OFF) -> VCC = 3.3 V (or 5 V), GND, module TXD -> GPIO 35, module RXD -> GPIO 22. Insert a FAT32 microSD card.
(For I2C instead, remove `-DPN532_UART=1` in platformio.ini and use SDA = GPIO 27, SCL = GPIO 22.)

## First use

1. Power on. The screen shows the **Muhax_logger** WiFi name, a random 8-character password, a QR code and `http://192.168.4.1`.
2. Join that network with a phone and fill in the wizard (admin login, company WiFi + Test, time zone, GitHub + Test, workers via *Tap card now*).
3. After *Save* the device restarts, joins the company WiFi and shows its address for 10 s (also on the **Info** screen: touch the idle screen).
4. Admin site: `http://<device-ip>` or `http://muhax-logger.local` (HTTP Basic login).

Recovery: if the saved WiFi is unusable for 2 min the setup network returns (taps keep working). Hold **BOOT** for 10 s
or use *System -> Return to setup mode* to force it. Neither erases workers or records.

## GitHub

Private repo + fine-grained token limited to that repo, **Contents: read and write**. Files uploaded:
`attendance/<device-id>/YYYY-MM-DD.csv` (whole day file, re-uploaded after taps with a delay, default 90 s) and
`config/workers-<device-id>.csv`. TLS is verified against the pinned **USERTrust ECC root** in
[src/github_ca.h](src/github_ca.h) (valid to 2038). Token expiry (max 1 year) is the thing most likely to break uploads: the
dashboard shows the error.

## Files on the SD card

`/muhax/workers.csv`, `/muhax/state.log` (last IN/OUT per worker), `/muhax/sync.json`, `/muhax/logs/YYYY-MM-DD.csv`,
`/muhax/logs/pending-time.csv` (taps made before the first time sync). Log columns:
`timestamp_local,epoch_utc,uid,employee_id,name,action,time_trusted,time_corrected,device_id,flag`
(`flag` = `MISSING_OUT_PREV` when the previous IN was never closed, or `TIME_UNKNOWN`).

## Decisions taken (open questions in the plan)

| Plan item | Chosen |
|---|---|
| Setup address | 192.168.4.1 (`SETUP_IP` in config.h) |
| Setup password | random 8 chars each time the setup network starts |
| SD missing | taps queued in internal flash (about 15 taps), moved to the SD when it is back |
| Several devices | one folder per device id in the repo |
| Timesheet summary | not in v1 (raw log + CSV export) |

## Deviations from the plan / things to know

- Web pages are embedded in flash (PROGMEM) instead of LittleFS: no filesystem upload step.
- Screen dimming is by inactivity (60 s), not by the light sensor, because the LDR polarity is unverified on this board.
- Touch is only used as "tap anywhere" (info screen), so no calibration is needed.
- Admin site is plain HTTP: use it on the private company network only.
- The PN532 library logs "Bus already started" once at boot; harmless.

## Status

Compiles for all three display variants. **Not yet tested on real hardware**: first run on the board, check the screen
variant, the PN532 read, the SD card, then the checklist in section 9 of the plan.
