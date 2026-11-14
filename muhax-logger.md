# MUHAX Logger

*NFC check-in / check-out logger for small companies*

**MUHAX Logger** is a small attendance terminal built on the **ESP32-2432S028R "CYD"** (ESP32 with a 2.8" touchscreen and a microSD slot) and a **PN532 NFC reader**, programmed in **C++** (Arduino framework, PlatformIO).

A worker taps an NFC card when arriving and again when leaving. The device records the **date and time** on the **microSD card** and also in an **online database: a private GitHub repository**. The first time it starts, it creates its own WiFi network, **Muhax_logger**, and shows the network name, its password and the setup page address on the screen. A short setup page collects the company WiFi, the workers and their cards, the GitHub repo and the admin login. After that, the device runs a **local website** on the company network (its address is shown on the screen) where the admin adds, edits and removes workers and **exports the data**.

Status: **brainstorming / planning**. Parts: CYD bought; PN532 and NFC cards chosen from mikroelectron.com.

---

## 1. The idea in one page

1. **First start:** no settings saved, so the device starts the WiFi network **Muhax_logger**. The screen shows the network name, the password and the setup address.
2. **Setup:** you join that network with a phone, open the setup address, and fill in the company WiFi and password, the workers and their NFC card IDs, the GitHub repo, and the admin username and password.
3. **Normal use:** the device joins the company WiFi, gets the time and date from the internet at every start and re-syncs every 2 hours (it has no battery), and shows a clock and date. Each tap writes a record to the SD card, shows a welcome message with the worker's name, ID and time, and uploads to GitHub.
4. **Admin:** from any phone or PC on the company network, open the device's address (shown on its screen), log in, and manage workers, view records and export the data.

---

## 2. Goals and scope

### Version 1 (the goal of this plan)

- Tap card -> record **who, date, time, IN or OUT** (the device decides IN or OUT by alternating).
- Screen always shows **time and date**; on a tap shows **Welcome**, **name**, **ID**, **time** and IN/OUT.
- Records saved on the **SD card** and uploaded to a **private GitHub repo**.
- **First-time setup** through the **Muhax_logger** network and a setup page.
- **Local admin website**: workers (add, edit, disable, remove), unregistered cards, records, **export**, settings.
- Feedback on every tap through the **screen** and the board's **RGB LED** (green for IN, red for OUT). No buzzer.

### Later (not version 1)

- Timesheet summary (hours per worker per day or month) as an export.
- Firmware update over WiFi, battery-backed clock, several devices per company, manual corrections with an audit trail, Arabic on the screen.

### Non-goals

Payroll, a mobile app, cloud accounts other than GitHub, fingerprint or face recognition.

---

## 3. Brainstorm: decisions and recommendations

| # | Question | Options | Recommendation |
|---|---|---|---|
| 1 | Language | Python (MicroPython) or C++ | **C++** (decided): mature CYD, PN532 and web libraries, more memory-efficient |
| 2 | Setup network name | Fixed name or name + device ID | **Muhax_logger** (your choice). Add a suffix only if you will run several devices at once |
| 3 | Setup network password | Fixed for all devices, or random each time setup starts | **Random 8 characters each time**, shown on the screen (and as a QR code). Anyone who can read the screen is physically there |
| 4 | Setup address | 192.168.1.1 or 192.168.4.1 (ESP32 default) or other | **Decide.** 192.168.1.1 works, but many routers use 192.168.1.x; while the device tests the company WiFi, two networks with the same address range can confuse it. 192.168.4.1 or 10.10.10.1 is safer. It is one line in the code |
| 5 | Where data lives | SD only, GitHub only, or both | **Both.** SD is the main store (works offline); GitHub is the online copy |
| 6 | How to upload | Append lines, or re-upload the whole day file | **Re-upload the whole day file from the SD** (small, a few KB). No merge logic, no duplicates, safe to retry. See 5.7 |
| 7 | Admin login | Username + password | **Username + password**, password stored only as a hash. HTTP Basic login in version 1 (plain HTTP on the local network) |
| 8 | Deleting a worker | Delete or disable | **Disable** keeps old records readable; allow a real delete too, with a warning |
| 9 | Forgot to tap out | Ignore, auto-close, or flag | **Flag**: if the last record is an IN from a previous day, a new tap is an IN and the old shift is marked "missing OUT" in the export |
| 10 | Arabic names | On screen / only in web | **Latin letters on the screen**; Arabic allowed in the web panel and exports (UTF-8) |
| 11 | Card IDs | Store UID as hex text | **Hex text, no separators** (for example `04A1B2C3D4E5F6`). Works for 4-byte and 7-byte cards |
| 12 | Several devices | One or many | One device per folder in the repo (`attendance/<device-id>/`), so devices never overwrite each other |
| 13 | Time (no battery on the CYD) | NTP only, RTC module, or phone time | **NTP at every start and every 2 hours** (your choice), taps made before the first sync are kept and corrected afterwards; **"Set time from my phone"** button as fallback; optional DS3231 module later. See 5.8 |

---

## 4. Hardware

### 4.1 Parts

| Part | Purpose | Status / price (JD) |
|---|---|---|
| ESP32-2432S028R "CYD" (ESP32-WROOM-32, 2.8" 240x320 resistive touch, microSD, RGB LED) | Brain, screen, storage slot, WiFi | **Bought** (listing price was 16.00) |
| PN532 NFC RFID reader/writer module | Reads cards (13.56 MHz) | Chosen, 9.00 |
| RFID Mifare Keychain 13.56 MHz | Worker tag | Chosen, 0.90 |
| NFC Card NTAG216 13.56 MHz | Worker tag (listed as card, described as an adhesive sticker) | Chosen, 1.50 |

The tag quantities are enough for **testing only**. Order one tag per worker plus spares later.

### 4.2 Still needed

| Item | Why | Notes |
|---|---|---|
| microSD card, 8 to 32 GB (FAT32) | Main record store | A name-brand card is more reliable |
| 4-pin 1.25 mm cable (to Dupont) | Connects the PN532 to the CYD side connector | Check whether one came with the board; otherwise buy one or solder |
| 5 V / 1 A USB charger + good USB cable | Power | A weak charger causes random resets |
| ID holders | Only for NTAG216 **stickers** | Do not stick them on metal |
| DS3231 clock module | Keeps time without internet | Optional; shares the PN532's two I2C wires |

### 4.3 Wiring

The CYD has very few free pins: **GPIO 22 and 27** (side connector CN1, with 3.3 V and GND) are used by the PN532, and GPIO 35 is input-only. Everything else is used by the screen, touch, SD card and RGB LED.

| PN532 | CYD (connector CN1) |
|---|---|
| VCC | 3.3 V |
| GND | GND |
| SDA | GPIO 27 |
| SCL | GPIO 22 |

- PN532 mode switches: **I2C** (check the table printed on the board).
- Power the PN532 from **3.3 V**, not 5 V: on some modules the I2C lines are pulled up to the supply voltage, and 5 V on the ESP32 pins can damage it.
- In code: `Wire.begin(27, 22);` (SDA, SCL).
- A DS3231 (optional) uses the same two wires (addresses differ: PN532 0x24, DS3231 0x68).

Usual internal pin map of the common CYD revision (verify before relying on it; revisions differ):

| Function | Pins |
|---|---|
| Display (SPI) | MISO 12, MOSI 13, SCK 14, CS 15, DC 2, backlight 21 |
| Touch (XPT2046, own SPI) | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| microSD (SPI) | CS 5, MOSI 23, MISO 19, SCK 18 |
| RGB LED (active low) | R 4, G 16, B 17 |
| Light sensor (LDR) | 34 (can auto-dim the backlight) |
| BOOT button | 0 |

---

## 5. Software design

### 5.1 Modes (state machine)

```
            no settings saved / WiFi lost 2 min
   +--------------------------------------------+
   v                                            |
SETUP MODE --(Save)--> RUNNING (idle <-> tap) --+
 (own WiFi                 ^  joins company WiFi,
  Muhax_logger)            |  NTP time, local website,
                           |  GitHub sync
   BOOT button held 10 s --+---> back to SETUP MODE (keeps workers and records)
```

### 5.2 Where data is stored

| Where | What | Why there |
|---|---|---|
| **microSD** `/muhax/logs/2026-11-12.csv` | One attendance file per day | Main record store; easy to read on a PC |
| **microSD** `/muhax/workers.csv` | `employee_id, uid, name, active` | Workers list |
| **microSD** `/muhax/logs/pending-time.csv` | Taps made before the first time sync after a start (they are moved into the daily files once the time is known) | Keeps early taps safe (section 5.8) |
| **microSD** `/muhax/sync.json` | Which days are uploaded, last file `sha` | Sync bookkeeping |
| **Internal flash** (Preferences / LittleFS) | WiFi name and password, GitHub details and token, admin user and password **hash**, settings | Secrets stay off the removable SD card |
| **Internal flash** | Web pages (HTML, CSS, JS) | Served by the local website |
| **GitHub repo** | `attendance/<device-id>/2026-11-12.csv` and a daily backup of `workers.csv` | Online copy |

Attendance file columns: `timestamp_local, epoch_utc, uid, employee_id, name, action, time_trusted, time_corrected, device_id`.

If the **SD card is missing**, the screen shows a warning; recommended behavior is to keep recording into a small queue in internal flash and move the records to the SD card when it is back (an open decision, section 10).

### 5.3 Tap logic

1. Poll the PN532 continuously; read the card **UID** only (card memory is not used).
2. Ignore the same UID for N seconds (default 5) and show "Already tapped".
3. Look up the UID in the workers list (active workers only).
   - **Unknown card:** orange screen with its UID, and the UID is kept in a short list so the admin can turn it into a worker from the panel.
   - **Known card:** if the worker's last record is IN (from today), record **OUT**; otherwise record **IN**. A leftover IN from a previous day is flagged "missing OUT" and the new tap is an IN.
4. Append the record to today's file on the SD card and close the file (so a power cut loses at most the record in progress). If the time has not been synced yet after a start, the record goes to `pending-time.csv` instead and is corrected later (section 5.8).
5. Show the welcome screen and the LED (green for IN, red for OUT) for about 3 seconds, then return to idle.
6. A **background task** uploads to GitHub, so the screen never waits for the internet.

### 5.4 Screens (320 x 240, landscape)

Large text, high contrast: green = IN, red = OUT, orange = warnings.

**Idle (always shown between taps):**

```
+------------------------------------------+
| MUHAX Logger            [WiFi][SD][Cloud]|
|                                          |
|                 10:15:42                 |
|           Thursday, 12 Nov 2026          |
|                                          |
|             Tap your card                |
|                                          |
| In now: 5          Last: Ahmad  IN 09:58 |
+------------------------------------------+
```

**Check-in (green background):**

```
+------------------------------------------+
|                 Welcome!                 |
|                                          |
|              Ahmad Khaled                |
|                ID: 1042                  |
|                                          |
|             ** CHECKED IN **             |
|                   09:58                  |
|          Thursday, 12 Nov 2026           |
+------------------------------------------+
```

**Check-out (red background):**

```
+------------------------------------------+
|                 Goodbye!                 |
|                                          |
|              Ahmad Khaled                |
|                ID: 1042                  |
|                                          |
|            ** CHECKED OUT **             |
|                   17:31                  |
|            Worked today: 7h 33m          |
+------------------------------------------+
```

**First-time setup (shown until setup is saved):**

```
+------------------------------------------+
|              MUHAX Logger                |
|            FIRST-TIME SETUP              |
|                                          |
|  1. Join WiFi:   Muhax_logger            |
|     Password:    7K3Q9XPD    [ QR code ] |
|  2. Open in browser:                     |
|     http://192.168.4.1                   |
|                                          |
|  Waiting for setup...                    |
+------------------------------------------+
```

**Info screen** (tap the idle screen; no destructive actions on it):

```
+------------------------------------------+
| INFO                                     |
| Company:  Acme Co                        |
| WiFi:     OfficeNet (connected)          |
| Admin:    http://192.168.0.57            |
|           http://muhax-logger.local      |
| Time:     synced 43 min ago (every 2 h)  |
| SD card:  OK  (12 days, 1284 records)    |
| GitHub:   synced 2 min ago               |
|                                [ Back ]  |
+------------------------------------------+
```

The device address is also shown for about 10 seconds after every start-up, so the admin can always find the website.

**While the time is not known yet** (first seconds after a start), the clock area shows "Getting time..." and the date is hidden; taps still work. **Other screens:** unknown card (orange), SD card missing, NFC reader not responding, WiFi lost (the Cloud icon turns into "offline"; taps still work), connecting to WiFi, setup saved.

**Screen notes:**

- The CYD's screen shows Latin text well; the default libraries do **not** shape Arabic. Keep names in English on the screen.
- Draw only what changes (for example the clock digits) so the display stays smooth and uses little memory.
- Use the light sensor to dim the backlight in dark rooms and wake it on a tap (optional).
- Touch is resistive and needs calibration; keep touch targets large.

### 5.5 Setup mode and the setup page

1. The ESP32 starts its own WiFi network (access point) named **Muhax_logger** with a WPA2 password (8 or more characters), and a fixed address (decision 4). The screen shows the name, the password, the address and a QR code that joins the network.
2. Connect a phone, open the address (a captive portal can make the page open automatically).
3. The **setup wizard** (mobile-friendly, one step per screen):

| Step | Content |
|---|---|
| 1. Admin account | Admin **username** and **password** (minimum length, typed twice) |
| 2. Company | Company name (shown on the screen and in exports) |
| 3. Company WiFi | Scan and pick the network, enter its password, **Test** button (the device connects while the setup network stays up and reports success or failure). Must be a **2.4 GHz** network |
| 4. Time | Time zone (default UTC+3 for Jordan), time re-sync interval (default 2 hours), and a "use my phone's time" button |
| 5. GitHub | Owner, **repo name**, branch, access token, and a **Test** button that checks the token can write to that repo (section 5.7) |
| 6. Workers | Table: employee ID, name, card ID. **"Tap card now"** button fills the card ID from the reader; or type or paste it. Can be skipped and done later in the admin panel |
| 7. Options | Double-tap seconds, LED on/off, screen dimming |
| 8. Review and save | The device saves everything, restarts, joins the company WiFi, and shows its address on the screen |

4. **Recovery:** if the saved WiFi cannot be joined for about 2 minutes (wrong password, router changed), the **Muhax_logger** network comes back so the settings can be fixed. Holding the board's **BOOT** button for about 10 seconds also returns to setup mode. Neither erases workers or records.

### 5.6 Admin website (runs on the device after setup)

Address: the IP shown on the screen (for example `http://192.168.0.57`), and `http://muhax-logger.local` on phones and computers that support mDNS. Login with the admin username and password.

| Page | What you can do |
|---|---|
| **Dashboard** | Who is inside now, today's taps, and status of time, SD card and GitHub sync |
| **Workers** | Add, **edit** (name, ID), **replace a lost card** (assign a new card ID to the same worker), **disable**, **delete**; "tap card now" fills the card ID; import and export the workers list as CSV |
| **Unregistered cards** | Cards that were tapped but are not registered, with a button to turn each into a worker |
| **Records** | Browse by date and worker; records with an untrusted time or "missing OUT" are marked |
| **Export** | Pick a date range (and optionally a worker) and **download CSV** (UTF-8 with a byte-order mark so Excel shows Arabic correctly). Later: a timesheet summary |
| **Settings** | Company name, WiFi, GitHub details, time zone, time re-sync interval, double-tap seconds, LED, screen, admin password |
| **System** | **Sync to GitHub now**, **Sync time now**, set time from my phone, restart, return to setup mode, factory reset (with a confirmation) |

Security basics: the password is stored only as a salted hash; the GitHub token is **never shown again** after it is saved (only replaced); the site is plain HTTP, so use it **only on the company's private network** and never expose it to the internet.

### 5.7 GitHub sync

- A **private repository**. Authentication with a **fine-grained personal access token** limited to **that one repo** with **Contents: read and write** only. It is typed in the setup page and stored in the device's flash, never in the code or the repo. It expires (at most a year), so note a renewal date.
- **Method:** GitHub REST API, "create or update file contents" over HTTPS. The SD card is the source of truth, so the device uploads the **whole day file** (a few KB) after taps, with a short delay (default 1 to 2 minutes, a setting) to avoid a flood of commits. To update an existing file GitHub needs its current `sha`; the device keeps the last `sha` in `sync.json` and re-reads it if GitHub reports a conflict.
- **Offline:** if there is no internet, nothing is lost; the SD card keeps all records and the upload catches up later. Days that were not uploaded are retried automatically.
- **Repo layout:** `attendance/<device-id>/2026-11-12.csv` and `config/workers.csv` (a daily backup of the workers list; no secrets). Do not edit the log files by hand in the repo; the device overwrites them.
- **TLS:** verify GitHub's certificate (certificate bundle or pinned root) instead of skipping verification; certificates change over time, so keep the firmware up to date.
- **Test button** in setup: checks that the repo exists and the token can write to it, without touching real data.
- Reading the data later: open the repo on GitHub, or download the daily CSVs.

### 5.8 Time

The CYD has **no battery clock**, so every power cut or restart loses the time. The device therefore gets the date and time from the internet (NTP) and keeps re-syncing:

1. **At every start:** as soon as the WiFi is connected, get the time from NTP (try more than one server, for example `pool.ntp.org` and `time.google.com`, and retry every 30 seconds until it works). Apply the time zone offset (Jordan is UTC+3).
2. **Every 2 hours:** re-sync from NTP while running. The interval is a setting (`time_sync_hours`, default **2**). The ESP32's own clock keeps time between syncs.
3. **If a re-sync fails** (internet down): keep running on the internal clock, keep the time marked as trusted for now, retry every few minutes, and show the problem on the info screen ("Time: last synced 3 h ago"). After a longer failure (default 24 hours) mark new records `time_trusted = 0`.
4. **Before the first sync after a start:** the idle screen shows "Getting time..." instead of the clock. A tap still works: the record is saved with the **seconds since start** and marked untrusted, and goes into `/muhax/logs/pending-time.csv`. When the first sync arrives, the device calculates the real time of each of those taps (current time minus the seconds that passed since the tap), moves them into the correct daily files, and marks them `time_corrected = 1`. A tap made seconds after power-up is therefore not lost or wrongly dated.
5. **If there is no internet at all** (router down for a long time): the admin can press **Set time from my phone** in the panel, which gives a trusted time until the next restart. An optional DS3231 clock module with a battery removes the problem completely.

### 5.9 Libraries to find and test

- Display: TFT_eSPI or LovyanGFX (the setup differs between the ILI9341 and ST7789 versions of the board, and colors may need inversion)
- Touch: XPT2046 (XPT2046_Touchscreen, or the one in LovyanGFX), with calibration
- NFC: Adafruit PN532 over I2C on custom pins
- SD card: built-in `SD` library on its own SPI bus
- Settings and JSON: `Preferences` or `LittleFS`, ArduinoJson
- Network: `WiFi` (AP and STA modes), a web server (`WebServer`, or `ESPAsyncWebServer`), `DNSServer` (captive portal), `ESPmDNS`
- GitHub: `HTTPClient` with `WiFiClientSecure`, base64 and SHA-256 (mbedTLS, built in)
- QR code on the setup screen: a small QR library (for example `QRCode`)
- Optional: RTClib (DS3231)

Library names and versions should be checked on the real board.

### 5.10 Memory and performance notes

The ESP32-WROOM has no extra PSRAM. Display, SD card, the web server and a TLS connection to GitHub share about 300 KB of RAM, and a TLS connection alone uses a large part of that. So: draw the screen without a full-screen buffer, upload in a separate task one request at a time, keep web pages small (served from flash), and test the screen and the upload running together early.

---

## 6. Security and privacy summary

- **Setup network** has a WPA2 password shown only on the screen, and runs only during setup or recovery.
- **Admin site:** username + password, password hashed, plain HTTP on the local network only.
- **GitHub token:** one repo, contents only, stored on the device, renewed before expiry, revoked if the device is lost. Someone with physical access to the board could extract it.
- **Card UIDs are not secret** and basic cards can be cloned, so a worker could tap for a friend. Acceptable for a small trusted team; a PIN on the touchscreen is a possible later option.
- **Personal data:** names, IDs and times are stored on GitHub's servers (a third party outside Jordan). Keep the repo private, store only what is needed, and tell workers what is recorded and where.
- Git history keeps old versions, so deleting someone's data later needs more than deleting a line.

---

## 7. 3D printed case

- Shape: a small **terminal**. Two layouts to consider: **screen on top, tap area below** (tall and narrow), or **screen and tap area side by side** (wide). Rough size is about 130 x 60 mm or 140 x 60 mm face, 25 to 30 mm deep. Measure the real parts first.
- **PN532 antenna** behind a thin plastic wall (about 1.5 to 2 mm), facing the front, **beside or below the screen, not stacked behind the CYD**: the CYD's metal and wiring would weaken the card reading. No metal or magnets near the antenna. PN532 module is about 42.7 x 40.4 mm.
- **Screen window** matching the visible screen area, with a lip so the glass does not rattle.
- Openings: **USB port**, a slot for the **microSD card** (so it can be removed without opening the case), access to **BOOT and RESET**, and a small window for the **light sensor**.
- Mounting: wall screw holes, or a tilted desk stand. **Decision needed: desk or wall.**
- Material: PLA indoors; PETG for sun or heat.
- Measure the real board with calipers before printing; CYD revisions differ by a millimetre or two.
- Tip: search Printables or Thingiverse for "ESP32-2432S028 case" and adapt one; only the PN532 compartment would be new.

---

## 8. Visualizing the project

| Tool | Use |
|---|---|
| The screen mockups in section 5.4 | Preview of every screen |
| Interactive screen preview (can be built as an HTML page) | Click through idle, tap, setup and info screens |
| Wokwi (browser) | Simulate the ESP32 and parts before they arrive (check whether it supports the CYD and PN532) |
| Tinkercad / OpenSCAD / Fusion 360 / FreeCAD | Model the case; OpenSCAD makes it easy to change measurements |
| Fritzing | Wiring diagram |

---

## 9. Build plan and tests

### Build plan

1. **Receive parts;** check the board (display chip, USB type, connectors and pins).
2. **PlatformIO project;** run a display and touch test sketch.
3. **SD card test:** create, append and read a file.
4. **PN532 test:** read a card UID over I2C on pins 27 and 22 and print it on the screen.
5. **RGB LED test** (green and red).
6. **Milestone 1, offline MVP:** idle screen, tap logic, welcome screens, records to the SD card, hard-coded test workers.
7. **Setup mode:** the Muhax_logger network, the setup screen with the QR code, the setup wizard, settings saved to flash.
8. **Running mode:** join the company WiFi, **NTP time at start and every 2 hours** (with the "Getting time..." screen and the pending-time correction), the local website with login, workers page, records, export.
9. **GitHub sync:** private repo, token, upload with retries, test offline behavior.
10. **Extras:** screen dimming, info screen, unregistered cards.
11. **Case:** measure, model, print, assemble, test card reading through the wall.
12. **Pilot:** a few workers for a week, check the files on the SD card and in the repo, fix problems.

### Test checklist

- [ ] First start creates the **Muhax_logger** network; the screen shows the name, password and address; the setup page opens
- [ ] Wrong company WiFi password in setup: clear error, and the Test button reports it
- [ ] Saved WiFi becomes unavailable: after about 2 minutes the Muhax_logger network returns and settings can be fixed
- [ ] After setup the screen shows the address and the admin site opens with the right login; a wrong password is refused
- [ ] Same card tapped twice quickly is counted once
- [ ] Tap, tap alternates IN and OUT; an IN left from yesterday is flagged
- [ ] Unknown card is rejected, shows its UID, and appears in the panel's unregistered list
- [ ] Add, edit, replace card, disable and delete worker work and survive a restart
- [ ] Records appear on the SD card with correct date and time, and open correctly in Excel (including Arabic names in the export)
- [ ] Records appear in the private repo; no duplicates after retries
- [ ] Internet unplugged: taps still work, the screen shows offline, records upload later
- [ ] SD card removed: clear error, no crash
- [ ] Power cut during use: no lost or damaged earlier records
- [ ] Every start: the device gets the time from the internet within a few seconds of joining the WiFi
- [ ] Time re-syncs every 2 hours (check the log or the info screen) and the clock does not jump or drift between syncs
- [ ] Tap made right after power-up (before the time is known) is saved, then gets the correct date and time after the first sync
- [ ] Power cut and restart: time is correct again after the first sync, with no battery fitted
- [ ] No internet at start-up: records marked untrusted; "set time from my phone" fixes it
- [ ] A failed re-sync does not stop taps; the info screen shows how long ago the last sync was
- [ ] Card reads reliably through the case wall; the screen stays smooth while uploading

---

## 10. Risks and open questions

| Item | Risk / question |
|---|---|
| Board revision | Display chip (ILI9341 or ST7789), backlight pin and connector pins differ; test with a basic sketch first |
| Memory | Display, web server, SD and TLS share one ESP32 without PSRAM; test them together early |
| Setup address | 192.168.1.1 vs 192.168.4.1; **you decide** (section 3, item 4) |
| Setup password | Random each time (recommended) or fixed; **you decide** |
| Plain HTTP admin site | Fine on a private network only |
| GitHub dependence | Assumed GitHub; another Git host needs a different API. Token renewal is needed |
| SD card missing | Queue in flash or refuse taps; **you decide** (recommended: queue) |
| Forgot to tap out | Flag as "missing OUT" (recommended); other rules possible |
| Several devices | One folder per device ID; not needed for the first device |
| 2.4 GHz WiFi only | The company router must offer a 2.4 GHz network |
| No battery clock | Time depends on the internet at every start; if the router and internet are both down after a power cut, taps are marked untrusted until the time is set (phone button or later DS3231) |
| Arabic text | Not on the CYD screen; fine in the web panel and exports |
| Sticker vs keychain vs card | Choose the tag type per company; stickers need a holder |
| Card cloning | Acceptable for trusted teams |
| Desk or wall case | Decide before modelling |
| Brownouts | A weak USB charger can reset the board when WiFi and the screen peak |

**Questions for you:**

1. Setup address: 192.168.1.1 as in your example, or a safer one such as 192.168.4.1?
2. Setup password: random each time, or one fixed password?
3. If the SD card is missing, should taps be queued in internal flash or refused?
4. Is this for one device or several? (affects the repo folders)
5. Desk or wall, and stickers or keychains?
6. Do you want the timesheet summary (hours per worker) in version 1, or only the raw log?

---

## 11. Rough cost per terminal

Known parts: CYD 16.00 + PN532 9.00 + 2 test tags 2.40 = about **27.40 JD** (listing prices), plus a microSD card, and a charger and cable. Extra tags are about 0.45 to 0.90 JD each, plus a few cents of filament for the case. GitHub private repos are free for personal accounts, so no storage cost is expected.
