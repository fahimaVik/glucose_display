# Glucose Display: Android setup app

The phone app that sets up the glucose display: validate your Nightscout, join
the device's setup hotspot with one tap, pick the WiFi it should use, and send
it all in one go. Material 3, dark-mode aware. It talks to the device's HTTP API
(see the main README for the endpoints).

`project/` is a complete, ready-to-build Gradle project (not loose files to drop
into a new project). Build it however you prefer.

## Build and install (command line)

Needs a Java 17+ JDK and the Android SDK (both ship with Android Studio; you do
not need to open the IDE). Point `JAVA_HOME` at a JDK and `sdk.dir` at your SDK
(`project/local.properties`), then:

```bash
cd project
gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The first build downloads the Android Gradle Plugin and Material dependency, so
it needs internet once. `local.properties` and the build output are gitignored.

## Or in Android Studio

Open the `project/` folder directly (File, Open). Let Gradle sync, plug in your
phone with USB debugging on, and press Run.

## Using it

1. **Nightscout** (phone on normal internet): enter your site URL and a
   read-only access token, tap **Check**. The app confirms the token reads
   glucose and pulls your units and target range from the site. Get a token from
   Nightscout: menu, Admin Tools, Subjects, add one with the `readable` role.
2. **Connect**: enter the setup hotspot password and tap **Connect to
   GlucoseSetup** (Android shows a one-time "Connect?" prompt). Or join
   `GlucoseSetup` manually in WiFi settings and use the manual **Find device**.
3. **WiFi**: tap **Scan**, tap your network, enter its password in the popup.
4. **Send setup**. The device joins WiFi, checks Nightscout, and the display
   comes to life. The phone's WiFi returns to normal on its own.

The home screen shows the device's live status, and **Set up / reconfigure**
re-runs the wizard on a device already in setup mode.

### If the app cannot reach the device (manual mode)

On the `GlucoseSetup` hotspot the phone has no internet, so Android may route app
traffic over mobile data instead. If **Find device** fails, turn **mobile data
off**. The one-tap **Connect to GlucoseSetup** avoids this by binding device
requests to the hotspot directly.

## Notes

- The device API is unauthenticated. Anyone on the WPA2-protected setup hotspot,
  or on your home WiFi, could reconfigure the device. Fine for a personal device
  on a home network; a shared device token is the natural way to lock it down.
- The setup hotspot name and password come from the firmware's `config.h`
  (`AP_SSID` / `AP_PASSWORD`).
