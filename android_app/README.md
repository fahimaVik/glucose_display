# Glucose display WiFi — Android app

A small app to manage the WiFi networks your glucose display remembers: see the
saved list (its history), add a new network, and delete old ones. It talks to
the device's HTTP API — the same `http://192.168.4.1/api/networks` you tested in
a browser.

There is no APK here, only source. You build it once in Android Studio (free)
and install it on your phone. These files are not a full project on their own —
you create an empty project and drop them in.

## What you need

- **Android Studio** (free): https://developer.android.com/studio
- A USB cable, or wireless debugging, to put the app on your phone

## Build steps

1. **New project** → **Empty Views Activity** (the plain one, *not* "Empty
   Activity" which is Compose). Then:
   - **Name:** Glucose WiFi
   - **Package name:** `com.example.glucosewifi`  ← use exactly this, it must
     match the first line of `MainActivity.kt`
   - **Language:** Kotlin
   - **Minimum SDK:** API 24 or higher
   - Finish, and let Gradle sync (first time takes a few minutes).

2. **Replace three files** with the ones in this folder. In Android Studio's
   Project view (top-left dropdown set to "Android"):

   | This file | Goes to |
   |---|---|
   | `MainActivity.kt` | `app/java/com.example.glucosewifi/MainActivity.kt` |
   | `activity_main.xml` | `app/res/layout/activity_main.xml` |
   | `AndroidManifest.xml` | `app/manifests/AndroidManifest.xml` |

   Open each file in this folder, copy all of it, and paste over the whole
   contents of the matching file in Android Studio.

3. **Run it.** Plug your phone in (with USB debugging enabled — search "enable
   USB debugging android" if you have not before), pick it in the device
   dropdown at the top, and press the green ▶ Run button. The app installs and
   opens on your phone.

   After the first install you can unplug and launch it like any app.

## Using it

The app reaches the device two ways:

- **Setup mode** — when the device cannot join any saved network it makes its
  own hotspot `GlucoseSetup` (password: whatever you set as `AP_PASSWORD`). Join that hotspot on your
  phone, leave the address field as `192.168.4.1`, and use the app.
- **Normal mode** — when the device is online on your WiFi, join that same WiFi
  on your phone. You would then put the device's IP (shown in the app when it is
  online) in the address field instead of `192.168.4.1`.

To add a network: type its name and password, tap **Save and connect**. The
device stores it and tries to join. If it succeeds, the display comes back to
life on the new network.

### Important: turn mobile data OFF

When your phone is on the `GlucoseSetup` hotspot it has no internet, so Android
tends to send app traffic over mobile data instead — and the device is not
reachable that way. **Toggle mobile data off** while using the app in setup mode.
This is the single most common reason the app "cannot reach the device". This is
the same thing you did to make it work in the browser.

## How it works

The device keeps its network list (newest first, up to 8) in its own flash
memory, so the history lives on the device and survives being unplugged — the
app reads and edits that list rather than holding the only copy. Passwords are
sent to the device but never sent back: the app can show which networks are
saved, but not their passwords.

## Notes and limits

- The API is unauthenticated. Anyone on the `GlucoseSetup` hotspot (which is
  WPA2-protected) or on your home WiFi could change the device's networks. For a
  personal device on your own network this is usually fine; if you want it
  locked down, that is a good follow-up.
- The hotspot name and password are set in the firmware's `config.h`
  (`AP_SSID` / `AP_PASSWORD`) — change them there and reflash if you like.
