# Nightscout Glucose Display

A desk display for continuous glucose monitor (CGM) readings, built on an ESP32
and a 6.1 cm touchscreen. It pulls the latest reading from a
[Nightscout](https://nightscout.github.io/) site over WiFi and shows it big and
colour-coded, with a trend arrow, a mood face, a 3-hour history graph, and a
daisy. WiFi networks are managed from a companion Android app, so there is no
reflashing to change networks.

![status: working](https://img.shields.io/badge/status-working-brightgreen)

---

## Contents

- [What it shows](#what-it-shows)
- [Prerequisites: a Nightscout server](#prerequisites-a-nightscout-server)
  - [How it was set up](#how-it-was-set-up)
  - [The token this project needs](#the-token-this-project-needs)
- [Hardware](#hardware)
  - [Bill of materials](#bill-of-materials)
  - [Two board quirks that cost hours](#two-board-quirks-that-cost-hours-read-these-first)
- [Wiring](#wiring)
  - [Display → ESP32](#display--esp32)
  - [Touch → ESP32](#touch--esp32-separate-bus)
- [Software](#software)
  - [Libraries](#libraries-auto-installed-from-platformioini)
  - [TFT_eSPI config gotcha](#tft_espi-is-configured-in-platformioini-not-user_setuph)
  - [Secrets: include/config.h](#secrets-includeconfigh)
- [Project structure](#project-structure)
- [Bring-up](#bring-up)
  - [Flash it](#1-flash-it)
  - [Verify the wiring with a colour test](#2-verify-the-wiring-with-a-colour-test-recommended-for-a-fresh-build)
  - [First real boot: touch calibration](#3-first-real-boot-touch-calibration)
- [Using it](#using-it)
  - [Controls](#controls)
  - [Powering it standalone](#powering-it-standalone)
- [Changing WiFi from your phone](#changing-wifi-from-your-phone)
  - [Flow](#flow)
  - [The Android app](#the-android-app)
  - [HTTP API](#http-api)
- [Security notes](#security-notes)
- [Troubleshooting](#troubleshooting)
- [Credits](#credits)

---

## What it shows

- **Large glucose value**, colour-coded green / amber / red by range
- **Trend arrow** (flat, 45°, single, double), from Nightscout's direction field
- **A mood face whose _corner_ encodes the state**, so you can read it across a
  room before you read a digit:
  - top-right, red, angry → **high**
  - bottom-right, green, happy → **in range**
  - bottom-left, blue, `x_x` → **low**
  - the face **blinks** at urgent levels
- **A daisy** at top centre (why not)
- **A 3-hour dot graph** with the in-range band shaded, toggled by an on-screen
  touch button
- **A staleness counter** that turns red past 15 minutes. A CGM reads every 5
  minutes, so a frozen number with no warning is dangerous; this makes stale
  data obvious.
- **Clear error states.** A failed fetch keeps the last reading (the age
  counter is the honest signal) until it's too old to trust, then shows *why*:
  auth failure, no network, TLS problem, etc.

---

## Prerequisites: a Nightscout server

This display is a *client*. It never talks to the CGM directly; it reads from a
[Nightscout](https://nightscout.github.io/) instance, a self-hosted web app that
stores your glucose data and exposes a REST API. You need one running before the
device is useful.

The data pipeline this project was built around:

```
Dexcom G6 transmitter
  -> phone (Dexcom G6 app, unchanged)
  -> Dexcom Share (cloud)
  -> Nightscout's built-in bridge (polls Share every ~5 min)
  -> Nightscout on Northflank  <->  MongoDB Atlas (storage)
  -> this display reads /api/v1/entries.json over HTTPS
```

### How it was set up

Hosting options and free tiers change often, so treat the official docs as
canonical and the details below as "what worked in mid-2026". Official guide:
https://nightscout.github.io/

1. **Dexcom Share follower account.** In the Dexcom G6 app: Settings → Share →
   turn on → invite a follower (a `you+nightscout@gmail.com` alias works fine),
   then open the invite email and create a Dexcom *Follow* account with a
   username and password. Those become `BRIDGE_USER_NAME` / `BRIDGE_PASSWORD`
   below. Use the follower account, never your main Dexcom login, so a leak can
   only expose data, not change your account.
2. **Database: MongoDB Atlas free tier.** Create an account, a project, and an
   **M0 free** cluster (AWS, a region near you, e.g. Frankfurt `eu-central-1`).
   Add a database user (autogenerate the password and save it), allow access
   under Network Access, and copy the connection string. It becomes Nightscout's
   `MONGO_CONNECTION`, with your database name appended (e.g. `.../nightscout`).
3. **Deploy Nightscout.** Fork `nightscout/cgm-remote-monitor` on GitHub and
   deploy it on **Northflank** (its free tier hosts Nightscout). Set these
   environment variables:
   - `API_SECRET`: a 12+ character passphrase (the admin password)
   - `MONGO_CONNECTION`: the Atlas connection string from step 2
   - `BRIDGE_USER_NAME`, `BRIDGE_PASSWORD`: the Dexcom follower credentials
   - `BRIDGE_SERVER=EU`: required outside the US, otherwise the bridge polls the
     US Share server and finds nothing
   - plus display settings such as `DISPLAY_UNITS` (`mg/dl` or `mmol`)
4. **Verify.** Open your Northflank Nightscout URL in a browser; readings should
   start appearing within a few minutes.

### The token this project needs

Once Nightscout is up, create a **read-only** access token for the display:
*Admin Tools → Subjects → add a subject with the `readable` role*. Its token is
what goes in `NS_TOKEN` in [`include/config.h`](include/config.h). Don't use
`API_SECRET` for the device; a read-only token can't modify your data and is
revocable on its own.

---

## Hardware

### Bill of materials

| Part | Notes |
|---|---|
| **HiLetgo 6.1 cm ILI9341 240×320 SPI TFT** with touch panel + pen, [link](https://amzn.eu/d/01ns5aVi) | ILI9341 display driver + XPT2046 resistive touch controller, 5V/3.3V tolerant |
| **Binghe ESP-32-WROOM-32 DevKit V1** (30-pin, CH340, USB-C) on a power baseboard, [link](https://amzn.eu/d/01wKuMpM) | The kit is a DevKit stacked on an expansion baseboard |
| **AZDelivery jumper wires** (M2M / F2M / F2F, 20 cm), [link](https://amzn.eu/d/0io5VrWr) | Female-to-female connect the display header to the ESP32 header |
| USB-C cable + any 5V USB charger | For power / flashing |

### Two board quirks that cost hours. Read these first.

1. **Power the board, and flash it, through the _DevKit's own USB-C port_** (the
   one on the small upper board, between the `EN` and `BOOT` buttons). The
   baseboard's bottom USB-C jack is silkscreened **`USB5V` and is power-only**:
   it lights the LED but enumerates nothing on USB, so you cannot flash through
   it. It's fine for running the finished device from a charger.
2. **This particular touch panel has a dead bottom-right corner.** The display
   works there; touch does not. Calibration and the on-screen button are placed
   to avoid it. If you build with a different panel, that constraint may not
   apply.

---

## Wiring

The display header carries the SPI display **and** the SPI touch controller.
They run on **two separate SPI buses** here (the display on ESP32 hardware SPI,
the touch controller on its own pins), which avoids having to split the shared
signal wires.

On the ESP32 DevKit / baseboard, each pin position is a **G / V / S** trio:
`S` = signal (the GPIO), `V` = power, `G` = ground. **Only the `S` row is the
GPIO.** Wiring a signal to the `V` row connects it to a power rail instead.

### Display → ESP32

| Display pin | ESP32 pin | GPIO |
|---|---|---|
| `VCC` | `3V3` | - |
| `GND` | `GND` | - |
| `CS`  | `D5`  | 5  |
| `RESET` | `D4` | 4 |
| `DC` (may be labelled `RS` / `A0`) | `D2` | 2 |
| `SDI` / `MOSI` | `D23` | 23 |
| `SCK` | `D18` | 18 |
| `LED` | `3V3` | - |
| `SDO` / `MISO` | `D19` | 19 |

### Touch → ESP32 (separate bus)

| Touch pin | ESP32 pin | GPIO |
|---|---|---|
| `T_CLK` | `D14` | 14 |
| `T_CS`  | `D25` | 25 |
| `T_DIN` | `D13` | 13 |
| `T_DO`  | `D27` | 27 |
| `T_IRQ` | *(not connected)* | - |

**Notes**
- `LED` is the backlight. No 3.3V there means a black screen even when
  everything else is correct.
- **GPIO12 is deliberately unused.** It's a flash-voltage strap pin; a device
  holding it at boot can stop the ESP32 from starting.
- The blue on-board LED is on GPIO2, shared with `TFT_DC`, so it flickers as the
  display updates. That's normal.

---

## Software

Built with [PlatformIO](https://platformio.org/) (VS Code extension). Board
`esp32doit-devkit-v1`, Arduino framework.

### Libraries (auto-installed from `platformio.ini`)

- `bodmer/TFT_eSPI`, the display driver
- `bblanchon/ArduinoJson`, for Nightscout JSON and network-list storage
- `PaulStoffregen/XPT2046_Touchscreen` (from GitHub, **not** the registry copy;
  the registry snapshot predates the custom-SPI-bus support this project needs)

### TFT_eSPI is configured in `platformio.ini`, not `User_Setup.h`

This is the single most common way to get a **blank screen that still compiles
cleanly**. TFT_eSPI only reads a `User_Setup.h` if `USER_SETUP_LOADED` is
defined and the file is on its include path; otherwise it silently falls back
to its bundled defaults, which are wired for an ESP8266. All display settings
here are passed as `build_flags` so they actually reach the compiler:

```ini
build_flags =
    -DUSER_SETUP_LOADED=1
    -DILI9341_DRIVER=1
    -DTFT_WIDTH=240
    -DTFT_HEIGHT=320
    -DTFT_MISO=19  -DTFT_MOSI=23  -DTFT_SCLK=18
    -DTFT_CS=5     -DTFT_DC=2     -DTFT_RST=4
    ...
    -DSPI_FREQUENCY=40000000
```

### Secrets: `include/config.h`

Copy the defines and fill in your own values. **This file is `.gitignore`d** so
your WiFi password and Nightscout token never reach version control.

```c
#define WIFI_SSID     "your-wifi"      // seeds the network list on first boot only
#define WIFI_PASSWORD "your-password"

#define NS_HOST   "your-site.example.com"     // host only, no https:// or trailing /
#define NS_TOKEN  "device-xxxxxxxxxxxx"       // read-only Nightscout access token

#define USE_MMOL  0                    // 0 = mg/dL, 1 = mmol/L

#define BG_URGENT_LOW  55              // thresholds in mg/dL regardless of display units
#define BG_LOW         80
#define BG_HIGH        180
#define BG_URGENT_HIGH 250

#define AP_SSID     "GlucoseSetup"     // setup-hotspot name...
#define AP_PASSWORD "choose-your-own"  // ...and password (min 8 chars)
#define MDNS_HOST   "glucose"          // reachable at http://glucose.local
```

**Get a read-only Nightscout token** from *Admin Tools → Subjects*: add a
subject with the `readable` role and use its token. Do **not** use your
`API_SECRET`; a read-only token is revocable and can't write to your data.

---

## Project structure

```
glucose_display/
├── platformio.ini          # board, libraries, TFT_eSPI display config
├── include/
│   └── config.h            # secrets + thresholds (gitignored)
├── src/
│   └── main.cpp            # firmware: display, touch, Nightscout, WiFi provisioning
└── android_app/
    ├── README.md           # how to build/install the app
    ├── MainActivity.kt     # app source (copy targets)
    ├── activity_main.xml
    ├── AndroidManifest.xml
    └── project/            # ready-to-build Gradle project (build output gitignored)
```

---

## Bring-up

### 1. Flash it

Connect the **DevKit's USB-C port** to your computer. It enumerates as a CH340
serial port. Then:

```bash
pio run -t upload
```

### 2. Verify the wiring with a colour test (recommended for a fresh build)

Before the full app, confirm the display and SPI wiring with a minimal sketch.
Temporarily replace `src/main.cpp` with this, flash, and watch:

```cpp
#include <Arduino.h>
#include <TFT_eSPI.h>
TFT_eSPI tft = TFT_eSPI();

void setup() {
  tft.init();
  tft.setRotation(1);
}
void loop() {
  tft.fillScreen(TFT_BLUE);  delay(1500);
  tft.fillScreen(TFT_RED);   delay(1500);
  tft.fillScreen(TFT_GREEN); delay(1500);
}
```

The screen should cycle **blue → red → green** forever.

- **Black screen:** backlight. Is `LED` on 3.3V? Then check `MOSI`/`SCK`/`DC`.
- **White/grey, no colour:** data not arriving. `MOSI` (D23), `SCK` (D18), or
  `DC` (D2).
- **Garbled / wrong colours:** usually `DC` or `RST`, or a loose jumper.

Then restore the real `src/main.cpp` and re-flash.

### 3. First real boot: touch calibration

On first boot (and only then) it runs a 2-point touch calibration: tap the
crosshair at **top-right**, then the one at **bottom-left**. The result is saved
to flash and never asked again (hold `BOOT` 3 s to redo it). This diagonal is
used deliberately because this panel's bottom-right corner is dead.

---

## Using it

### Controls

- **On-screen graph button** (left edge, middle): tap to show/hide the 3-hour
  graph.
- **`BOOT` button:** short press also toggles the graph; **hold 3 s** to
  re-run touch calibration. (Only press it while running. Holding it during a
  reset enters flash-download mode.)

Both preferences persist across power cycles.

### Powering it standalone

No computer needed. Feed 5V into the **baseboard's `USB5V` jack** from any phone
charger or power bank. **Use one power source at a time:** charger *or* the
DevKit USB port, never both.

---

## Changing WiFi from your phone

WiFi credentials are **not** hardcoded (after the first-boot seed). The device
keeps a list of up to 8 networks in flash (its history), tries them
newest-first on boot, and if none are reachable it starts its own **setup
hotspot**.

### Flow

1. When no saved network is found, the display shows **Setup mode** and starts
   the hotspot **`GlucoseSetup`** (password: the one you set as `AP_PASSWORD`).
2. On your phone, join that hotspot, and **turn mobile data off** (a no-internet
   hotspot otherwise gets bypassed over cellular, the most common gotcha).
3. Open the **Glucose WiFi** app (or a browser at `http://192.168.4.1`), and add
   your network. The device stores it, joins it, and the display returns.

When the device is online on your WiFi it's also reachable at
`http://glucose.local` (mDNS) or its IP.

### The Android app

Source and a build/usage guide are in [`android_app/`](android_app/). It's a
small native Kotlin app: see saved networks, add one, delete one. It can be
built and installed entirely from the command line against an existing Android
Studio SDK/JDK (`gradle assembleDebug`, then `adb install`), no IDE session
required. See [`android_app/README.md`](android_app/README.md).

### HTTP API

Plain JSON, served in both setup-hotspot and normal modes. Passwords are sent to
the device but never returned.

| Method | Path | Body / query | Purpose |
|---|---|---|---|
| `GET` | `/api/networks` | - | status + saved SSIDs (history) |
| `POST` | `/api/networks` | `{"ssid","pass"}` | add a network and try to join |
| `DELETE` | `/api/networks` | `?ssid=NAME` | forget a network |
| `GET` | `/api/scan` | - | nearby SSIDs |

---

## Security notes

- **Nightscout is fetched over verified HTTPS.** The device checks the site's
  certificate against the framework's Mozilla CA bundle; there is deliberately
  **no insecure fallback**, so a failed handshake shows an error rather than
  trusting the connection. The token is sent in a header, not the URL, so it
  stays out of server logs.
- **The provisioning HTTP API is unauthenticated.** Anyone on the WPA2-protected
  setup hotspot, or on your home WiFi, could change the device's networks.
  Acceptable for a personal device on a home network; lock it down if that
  matters to you.
- Secrets live only in `include/config.h` (gitignored). WiFi passwords and the
  Nightscout token are compiled into the firmware image in plaintext, so physical
  possession of the board exposes them. Treat a lost device accordingly
  (rotate the token, which is why it's read-only and revocable).

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Compiles fine, **screen stays black** | `USER_SETUP_LOADED` / display config not reaching the library; or `LED` not on 3.3V |
| **Can't flash / no serial port** | Cable is in the baseboard `USB5V` (power-only) jack; use the DevKit's own USB-C port |
| Board **won't boot** after wiring touch | A touch wire on the `V` (power) row instead of `S`; or something on GPIO12 |
| **Touch doesn't respond** in one area | This panel's bottom-right corner is dead; keep controls elsewhere |
| App **"can't reach device"** on the hotspot | Turn mobile data off; make sure the URL is `http://` not `https://` |
| Reading looks **stuck** | Check the age counter; red = stale (>15 min), a source/upstream problem, not the display |

---

## Credits

Built for a personal CGM setup fed by a self-hosted Nightscout instance.
