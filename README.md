# MUHAX Logger QR (firmware)

PlatformIO project for the ESP32-2432S028R "CYD". Open the folder in VS Code + PlatformIO, build and upload (`env:cyd`).

## First run
1. Insert a FAT32 microSD card, power the board.
2. Join WiFi **Muhax_logger** (password shown on screen), open `http://192.168.4.1`, fill the form, save.
3. The board restarts, joins the company WiFi, shows its address for 6 s, then the QR.
4. Admin: `http://<ip>/admin` (or `http://muhax-logger.local/admin`).

## Buttons
- BOOT short press: info screen (with Join-WiFi QR). BOOT held 10 s: setup mode (workers and records are kept).

## What is implemented
Rotating QR (HMAC, 30 s, accepts current + previous window), presence pass, ID+PIN login with salted hash, remember-this-phone cookie, lockout after 5 tries, double-tap protection, IN/OUT alternation with MISSING_OUT flag, SD logs per day, background GitHub upload of the whole day file (private repo, token in flash), NTP at start and every N hours, admin site (dashboard, workers, records, CSV export with BOM, system), setup AP + captive portal, recovery to setup after 2 min WiFi loss, RGB LED feedback.

## Deliberate simplifications (v1 of the code)
- Setup is one form page, not a step-by-step wizard, and has no "Test" buttons for WiFi/GitHub. Settings are edited by returning to setup mode (blank passwords keep saved values).
- No touchscreen use (info screen is on the BOOT button) to avoid SPI conflicts between touch, display and SD.
- SD card missing: check-ins are refused with an error (no flash queue yet).
- GitHub TLS: `GITHUB_INSECURE_TLS 1` in main.cpp skips certificate checks. Replace with a root-CA / certificate bundle before real use.
- No fixed-IP option (use a DHCP reservation), no DuckDNS, no screen dimming.

## Not compiled or tested on hardware
Check first: display driver/colours (ILI9341 vs ST7789, see platformio.ini comments), SD on VSPI pins, memory with TLS + several phones.
