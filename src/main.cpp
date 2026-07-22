#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <TFT_eSPI.h>

#include "config.h"

TFT_eSPI tft = TFT_eSPI();

// Mozilla root CA bundle. Already linked into the framework, so verifying
// Nightscout's certificate costs no extra flash and survives Let's Encrypt
// rotating their chain - which pinning a single root would not.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

struct Reading {
    bool    valid = false;
    int     sgv   = 0;       // always mg/dL, as Nightscout stores it
    String  direction;
    int64_t dateMs = 0;
};

enum Band { BAND_LOW, BAND_OK, BAND_HIGH };

// Why a fetch failed, so the screen can say something more useful than
// "no data". There is deliberately no insecure fallback: if the certificate
// does not verify we show an error rather than trusting the connection.
enum FetchResult {
    FETCH_OK,
    FETCH_NO_WIFI,
    FETCH_TLS,      // could not establish a verified connection
    FETCH_AUTH,     // 401/403 - token rejected
    FETCH_HTTP,     // any other non-200
    FETCH_DATA      // connected fine, but the payload was unusable
};

// ---- palette ------------------------------------------------------------
// RGB565. The foreground colours are darkened versions of the obvious ones:
// against a light background, bright green and orange sit at nearly the same
// luminance as the pink and the digits stop reading at a glance.

static const uint16_t C_BG         = 0xFE19;  // #FFC0CB light pink
static const uint16_t C_IN_RANGE   = 0x03E0;  // #007F00 dark green
static const uint16_t C_OUT_RANGE  = 0xB2E0;  // #B35C00 burnt amber
static const uint16_t C_URGENT     = 0xC000;  // #C00000 deep red
static const uint16_t C_LABEL      = 0x528A;  // #505050 dark grey

static const uint16_t C_PETAL      = TFT_WHITE;
static const uint16_t C_PETAL_EDGE = 0x7BEF;  // white on pink needs an outline
static const uint16_t C_CENTRE     = 0xFE80;  // #FFD200 golden

static const uint16_t C_GRAPH_BAND = 0xFFFF;  // in-range zone behind the graph

static const uint16_t C_BTN_OFF_FILL = 0xFF3D;  // #FFE6EA dimmed button face
static const uint16_t C_BTN_OFF_EDGE = 0xA451;  // #A08A8E dimmed button outline

static const uint16_t C_FACE_HIGH  = 0xF800;  // red
static const uint16_t C_FACE_OK    = 0x0580;  // #00B000 medium green
static const uint16_t C_FACE_LOW   = 0x001F;  // blue
static const uint16_t C_OUTLINE    = TFT_BLACK;

// ---- layout -------------------------------------------------------------
// Regions are cleared and redrawn individually rather than wiping the whole
// screen, so the daisy and the face never flicker when the minute counter
// ticks. They are laid out not to overlap; changing one means checking it
// still clears the whole of what it draws and nothing of what it does not.

static const int DAISY_CX = 160, DAISY_CY = 34;

static const int VAL_X = 26, VAL_Y = 62, VAL_W = 254, VAL_H = 80;
static const int VAL_TEXT_X = 218, VAL_TEXT_Y = 102;
static const int ARROW_CX = 258, ARROW_CY = 102;

static const int AGE_X = 66, AGE_Y = 208, AGE_W = 188, AGE_H = 28;
static const int AGE_TEXT_Y = 232;

// The graph lives in the band between the big number and the age line. It is
// narrower than the screen because the two bottom faces intrude below y=177.
static const int GRAPH_MAX_PTS = 36;    // 36 readings x 5 min = 3 hours
static const int GRAPH_X = 60, GRAPH_Y = 148, GRAPH_W = 200, GRAPH_H = 56;
static const int GRAPH_LO = 40, GRAPH_HI = 300;   // mg/dL, values are clamped

static const int FACE_R = 20;
static const int FACE_HI_CX = 286, FACE_HI_CY = 38;   // high    -> top right
static const int FACE_OK_CX = 286, FACE_OK_CY = 198;  // in range-> bottom right
// The low face sits lower than its siblings to clear the graph button, which
// occupies the space it would otherwise have taken.
static const int FACE_LO_CX =  30, FACE_LO_CY = 210;  // low     -> bottom left

// Graph toggle button, left of the graph and vertically centred on it. The
// touch target is deliberately much larger than the drawn button: 42x30 px is
// about 6x4.5mm, far smaller than a fingertip on a resistive panel.
static const int BTN_X = 7, BTN_Y = 155, BTN_W = 42, BTN_H = 30;
static const int HIT_X0 = 0, HIT_Y0 = 141, HIT_X1 = 58, HIT_Y1 = 189;

// Calibration targets, in screen coordinates. Deliberately the top-right /
// bottom-left diagonal: profiling this panel showed its bottom-right corner
// is dead - across 23 taps it never once produced a reading - so the obvious
// top-left / bottom-right pair can never complete. These two differ in both
// axes, which is all a two point calibration needs.
static const int CAL_X0 = 280, CAL_Y0 =  40;   // top right
static const int CAL_X1 =  40, CAL_Y1 = 200;   // bottom left

// Bumped whenever the calibration routine changes in a way that invalidates
// stored values, which forces a one-time recalibration on the next boot.
static const int CAL_VERSION = 3;

// ---- what is currently on the screen ------------------------------------

// Newest first, matching the order Nightscout returns entries in.
struct History {
    int     sgv[GRAPH_MAX_PTS];
    int     count    = 0;
    int64_t newestMs = 0;
};

static Reading  lastReading;
static History  history;
static uint32_t lastPollMs = 0;
static int64_t  shownHistoryMs = -1;

// The BOOT button doubles as a user button once the board is running. GPIO0 is
// a strap pin, so it is only safe to read it after boot - never drive it.
static const int BOOT_BTN_PIN = 0;

static Preferences prefs;
static bool        graphOn = true;

// The touch controller gets its own SPI bus rather than sharing the display's.
// The display already occupies the S row on 18/19/23, and a second bus on free
// pins is simpler than splitting those signals. D12 is deliberately avoided:
// it is a flash-voltage strap pin and a device holding it high stops the board
// from booting.
static const int TOUCH_CLK  = 14;
static const int TOUCH_MOSI = 13;
static const int TOUCH_MISO = 27;
static const int TOUCH_CS   = 25;

static SPIClass            touchSPI(HSPI);
static XPT2046_Touchscreen ts(TOUCH_CS);

static bool   chromeDrawn   = false;
static int    shownSgv      = -1;
static String shownDir      = "";
static int    shownAge      = -9999;
static Band   shownFaceBand = BAND_OK;
static bool   shownFaceOn   = false;

// ---------------------------------------------------------------- helpers

static Band bandFor(int sgv) {

    if (sgv < BG_LOW)  return BAND_LOW;
    if (sgv > BG_HIGH) return BAND_HIGH;

    return BAND_OK;
}

static bool isUrgent(int sgv) {
    return sgv <= BG_URGENT_LOW || sgv >= BG_URGENT_HIGH;
}

static uint16_t colourFor(int sgv) {

    if (isUrgent(sgv))           return C_URGENT;
    if (bandFor(sgv) != BAND_OK) return C_OUT_RANGE;

    return C_IN_RANGE;
}

// Nightscout stores mg/dL; convert only for display.
static String formatValue(int sgv) {

#if USE_MMOL
    char buf[8];
    dtostrf(sgv / 18.0, 0, 1, buf);
    return String(buf);
#else
    return String(sgv);
#endif
}

static int minutesAgo(int64_t dateMs) {

    time_t now = time(nullptr);
    if (now < 1700000000) return -1;   // clock not synced yet

    return (int)((now - (time_t)(dateMs / 1000)) / 60);
}

// ---------------------------------------------------------------- drawing

// Thick line built from overlapping dots. Avoids drawWideLine's anti-aliasing,
// which needs to know the background colour and fringes badly over a face.
static void thickLine(int x1, int y1, int x2, int y2, int r, uint16_t colour) {

    const int steps = max(abs(x2 - x1), abs(y2 - y1)) * 2 + 1;

    for (int i = 0; i <= steps; i++) {
        const float t = (float)i / steps;
        tft.fillCircle(x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, r, colour);
    }
}

// Parabolic mouth. bend > 0 curves the middle downwards (a smile),
// bend < 0 lifts it (a frown).
static void drawMouth(int cx, int cy, int halfWidth, int bend, uint16_t colour) {

    for (int i = 0; i <= 18; i++) {
        const float t = -1.0 + 2.0 * i / 18.0;
        tft.fillCircle(cx + t * halfWidth, cy + bend * (1.0 - t * t), 2, colour);
    }
}

static void drawDaisy(int cx, int cy) {

    for (int i = 0; i < 8; i++) {
        const float a  = i * PI / 4.0;
        const int   px = cx + cos(a) * 13;
        const int   py = cy + sin(a) * 13;

        tft.fillCircle(px, py, 7, C_PETAL);
        tft.drawCircle(px, py, 7, C_PETAL_EDGE);
    }

    tft.fillCircle(cx, cy, 6, C_CENTRE);
}

// Faces are outlined so they read as sprites rather than colour blobs, and so
// the red one does not bleed into the pink around it.
static void faceBase(int cx, int cy, uint16_t fill) {

    tft.fillCircle(cx, cy, FACE_R, fill);
    tft.drawCircle(cx, cy, FACE_R,     C_OUTLINE);
    tft.drawCircle(cx, cy, FACE_R - 1, C_OUTLINE);
}

static void drawFaceHigh(int cx, int cy) {

    faceBase(cx, cy, C_FACE_HIGH);

    tft.fillCircle(cx - 7, cy - 3, 3, TFT_BLACK);
    tft.fillCircle(cx + 7, cy - 3, 3, TFT_BLACK);

    thickLine(cx - 13, cy - 13, cx - 4, cy - 8, 1, TFT_BLACK);
    thickLine(cx + 13, cy - 13, cx + 4, cy - 8, 1, TFT_BLACK);

    drawMouth(cx, cy + 12, 9, -7, TFT_BLACK);
}

static void drawFaceOk(int cx, int cy) {

    faceBase(cx, cy, C_FACE_OK);

    tft.fillCircle(cx - 7, cy - 5, 3, TFT_BLACK);
    tft.fillCircle(cx + 7, cy - 5, 3, TFT_BLACK);

    drawMouth(cx, cy + 3, 10, 8, TFT_BLACK);
}

static void drawFaceLow(int cx, int cy) {

    faceBase(cx, cy, C_FACE_LOW);

    // x_x - white features, because black on pure blue is nearly invisible.
    thickLine(cx - 11, cy - 8, cx - 3, cy,     1, TFT_WHITE);
    thickLine(cx -  3, cy - 8, cx - 11, cy,    1, TFT_WHITE);
    thickLine(cx +  3, cy - 8, cx + 11, cy,    1, TFT_WHITE);
    thickLine(cx + 11, cy - 8, cx +  3, cy,    1, TFT_WHITE);

    thickLine(cx - 7, cy + 9, cx + 7, cy + 9,  1, TFT_WHITE);
}

static int graphY(int sgv) {

    if (sgv > GRAPH_HI) sgv = GRAPH_HI;
    if (sgv < GRAPH_LO) sgv = GRAPH_LO;

    return GRAPH_Y + (int64_t)(GRAPH_HI - sgv) * (GRAPH_H - 1) / (GRAPH_HI - GRAPH_LO);
}

// Dots rather than a joined line: a CGM drops readings often enough that a
// line would invent data across the gaps.
static void drawGraph() {

    tft.fillRect(GRAPH_X, GRAPH_Y, GRAPH_W, GRAPH_H, C_BG);

    const int yHigh = graphY(BG_HIGH);
    const int yLow  = graphY(BG_LOW);

    tft.fillRect(GRAPH_X, yHigh, GRAPH_W, yLow - yHigh + 1, C_GRAPH_BAND);

    // history[0] is the newest reading, so it plots at the right-hand edge.
    for (int i = 0; i < history.count; i++) {
        const int x = GRAPH_X + GRAPH_W - 1 -
                      (i * (GRAPH_W - 1)) / (GRAPH_MAX_PTS - 1);

        tft.fillCircle(x, graphY(history.sgv[i]), 2, colourFor(history.sgv[i]));
    }
}

// A miniature of the graph it controls: same dots, same colours, including one
// amber to stand for an out-of-range reading. Dimmed when the graph is hidden,
// so the button also reports the current state.
static void drawGraphButton() {

    const uint16_t edge = graphOn ? C_IN_RANGE : C_BTN_OFF_EDGE;
    const uint16_t fill = graphOn ? TFT_WHITE  : C_BTN_OFF_FILL;

    tft.fillRoundRect(BTN_X, BTN_Y, BTN_W, BTN_H, 7, fill);
    tft.drawRoundRect(BTN_X,     BTN_Y,     BTN_W,     BTN_H,     7, edge);
    tft.drawRoundRect(BTN_X + 1, BTN_Y + 1, BTN_W - 2, BTN_H - 2, 6, edge);

    const int px[4] = { BTN_X +  9, BTN_X + 18, BTN_X + 27, BTN_X + 35 };
    const int py[4] = { BTN_Y + 22, BTN_Y + 15, BTN_Y + 18, BTN_Y +  9 };

    for (int i = 0; i < 3; i++) {
        tft.drawLine(px[i], py[i], px[i + 1], py[i + 1], edge);
    }

    for (int i = 0; i < 4; i++) {
        const uint16_t dot = (graphOn && i == 2) ? C_OUT_RANGE : edge;
        tft.fillCircle(px[i], py[i], 2, dot);
    }
}

static void faceCentre(Band band, int &cx, int &cy) {

    switch (band) {
        case BAND_HIGH: cx = FACE_HI_CX; cy = FACE_HI_CY; break;
        case BAND_LOW:  cx = FACE_LO_CX; cy = FACE_LO_CY; break;
        default:        cx = FACE_OK_CX; cy = FACE_OK_CY; break;
    }
}

static void clearFace(Band band) {

    int cx, cy;
    faceCentre(band, cx, cy);

    tft.fillRect(cx - FACE_R - 1, cy - FACE_R - 1,
                 FACE_R * 2 + 2,  FACE_R * 2 + 2, C_BG);
}

// Static parts: background, units label, daisy. Drawn once per screen layout.
static void drawChrome() {

    tft.fillScreen(C_BG);

    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString(USE_MMOL ? "mmol/L" : "mg/dL", 12, 10, 2);

    drawDaisy(DAISY_CX, DAISY_CY);
    drawGraphButton();

    chromeDrawn    = true;
    shownSgv       = -1;
    shownDir       = "";
    shownAge       = -9999;
    shownFaceOn    = false;
    shownHistoryMs = -1;
}

static void drawArrow(int cx, int cy, float angleDeg, uint16_t colour) {

    const float rad = angleDeg * PI / 180.0;
    const int   len = 26;

    const float dx = cos(rad), dy = -sin(rad);

    const int x1 = cx - dx * len / 2, y1 = cy - dy * len / 2;
    const int x2 = cx + dx * len / 2, y2 = cy + dy * len / 2;

    thickLine(x1, y1, x2, y2, 1, colour);

    const int hx = dx * 11, hy = dy * 11;
    const int wx = -dy * 8,  wy = dx * 8;

    tft.fillTriangle(x2, y2,
                     x2 - hx + wx, y2 - hy + wy,
                     x2 - hx - wx, y2 - hy - wy, colour);
}

static void drawTrend(int cx, int cy, const String &dir, uint16_t colour) {

    if (dir == "Flat")          { drawArrow(cx, cy,   0, colour); return; }
    if (dir == "FortyFiveUp")   { drawArrow(cx, cy,  45, colour); return; }
    if (dir == "FortyFiveDown") { drawArrow(cx, cy, -45, colour); return; }
    if (dir == "SingleUp")      { drawArrow(cx, cy,  90, colour); return; }
    if (dir == "SingleDown")    { drawArrow(cx, cy, -90, colour); return; }

    if (dir == "DoubleUp") {
        drawArrow(cx - 9, cy, 90, colour);
        drawArrow(cx + 9, cy, 90, colour);
        return;
    }
    if (dir == "DoubleDown") {
        drawArrow(cx - 9, cy, -90, colour);
        drawArrow(cx + 9, cy, -90, colour);
        return;
    }

    // "NONE", "NOT COMPUTABLE", "RATE OUT OF RANGE" or anything unexpected
    tft.setTextColor(colour, C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("?", cx, cy, 4);
}

static void updateValue(const Reading &r) {

    if (r.sgv == shownSgv && r.direction == shownDir) return;

    const uint16_t colour = colourFor(r.sgv);

    tft.fillRect(VAL_X, VAL_Y, VAL_W, VAL_H, C_BG);

    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(colour, C_BG);
    tft.drawString(formatValue(r.sgv), VAL_TEXT_X, VAL_TEXT_Y, 8);

    drawTrend(ARROW_CX, ARROW_CY, r.direction, colour);

    shownSgv = r.sgv;
    shownDir = r.direction;
}

static void updateAge(int age) {

    if (age == shownAge) return;

    tft.fillRect(AGE_X, AGE_Y, AGE_W, AGE_H, C_BG);

    tft.setTextDatum(BC_DATUM);

    if (age < 0) {
        tft.setTextColor(C_LABEL, C_BG);
        tft.drawString("syncing clock", tft.width() / 2, AGE_TEXT_Y, 2);
    } else {
        // A CGM reads every 5 min, so past 15 the number on screen is not
        // something to act on. Say so rather than showing it plainly.
        const bool stale = age > 15;
        tft.setTextColor(stale ? C_URGENT : C_LABEL, C_BG);

        char buf[32];
        snprintf(buf, sizeof(buf), stale ? "STALE - %d min ago" : "%d min ago", age);
        tft.drawString(buf, tft.width() / 2, AGE_TEXT_Y, 2);
    }

    shownAge = age;
}

static void updateFace(Band band, bool on) {

    if (band == shownFaceBand && on == shownFaceOn) return;

    if (shownFaceOn) clearFace(shownFaceBand);

    if (on) {
        int cx, cy;
        faceCentre(band, cx, cy);

        switch (band) {
            case BAND_HIGH: drawFaceHigh(cx, cy); break;
            case BAND_LOW:  drawFaceLow(cx, cy);  break;
            default:        drawFaceOk(cx, cy);   break;
        }
    }

    shownFaceBand = band;
    shownFaceOn   = on;
}

static void refresh(const Reading &r) {

    if (!chromeDrawn) drawChrome();

    updateValue(r);
    updateAge(minutesAgo(r.dateMs));

    if (graphOn && history.count > 0 && history.newestMs != shownHistoryMs) {
        drawGraph();
        shownHistoryMs = history.newestMs;
    }

    // Outside the urgent bands the face is simply always on. Inside them it
    // blinks, so that 250 does not look the same as 190 from across the room.
    const bool on = !isUrgent(r.sgv) || ((millis() / 500) % 2 == 0);

    updateFace(bandFor(r.sgv), on);
}

// The choice is kept in NVS so a power cycle does not undo it.
static void toggleGraph() {

    graphOn = !graphOn;
    prefs.putBool("graph", graphOn);

    Serial.printf("graph %s\n", graphOn ? "on" : "off");

    // Full relayout: turning the graph off has to wipe the band it occupied,
    // and drawChrome resets the shown-state so everything else redraws too.
    if (lastReading.valid) {
        drawChrome();
        refresh(lastReading);
    }
}

// ---- touch calibration --------------------------------------------------
// Two points are enough for the linear map the XPT2046 needs. Stored in NVS,
// so this runs once ever unless the BOOT button is held to redo it.

static int  calRawX0 = 0, calRawY0 = 0, calRawX1 = 0, calRawY1 = 0;
static bool touchCalibrated = false;

static void drawCrosshair(int x, int y, uint16_t colour) {

    tft.drawLine(x - 12, y, x + 12, y, colour);
    tft.drawLine(x, y - 12, x, y + 12, colour);
    tft.drawCircle(x, y, 7, colour);
}

// Blocks until the panel is tapped and released. Gives up after 30s so a dead
// panel cannot strand the device on the calibration screen forever.
//
// The first sample after touch-down is unusable: the resistive plates are still
// settling and the reading can be wildly off - that is what broke the earlier
// calibration. So we discard the leading samples and average the rest.
static bool captureTap(int &rx, int &ry) {

    while (ts.touched()) delay(10);          // ignore a finger already down
    delay(200);

    const uint32_t deadline = millis() + 90000;

    while (millis() < deadline && !ts.touched()) delay(10);
    if (!ts.touched()) return false;

    delay(80);                               // let the plates settle

    int32_t sumX = 0, sumY = 0;
    int     n = 0;

    const uint32_t until = millis() + 250;

    while (millis() < until && ts.touched()) {
        const TS_Point p = ts.getPoint();
        sumX += p.x;
        sumY += p.y;
        n++;
        delay(10);
    }

    if (n < 3) return false;                 // lifted too fast to trust

    rx = sumX / n;
    ry = sumY / n;

    while (ts.touched()) delay(10);
    delay(200);

    Serial.printf("cal point raw x=%d y=%d (n=%d)\n", rx, ry, n);
    return true;
}

static void calibrationScreen(int x, int y, const char *step) {

    tft.fillScreen(C_BG);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(C_LABEL, C_BG);
    tft.drawString("Tap the cross", 160, 112, 4);

    tft.setTextColor(C_OUT_RANGE, C_BG);
    tft.drawString(step, 160, 142, 2);

    drawCrosshair(x, y, C_URGENT);
}

static void calibrateTouch() {

    Serial.println("calibration: waiting for point 1 of 2");
    calibrationScreen(CAL_X0, CAL_Y0, "1 of 2");

    if (!captureTap(calRawX0, calRawY0)) {
        Serial.println("calibration: point 1 timed out");
        touchCalibrated = false;
        return;
    }

    Serial.println("calibration: waiting for point 2 of 2");
    calibrationScreen(CAL_X1, CAL_Y1, "2 of 2");

    if (!captureTap(calRawX1, calRawY1)) {
        Serial.println("calibration: point 2 timed out");
        touchCalibrated = false;
        return;
    }

    // Two taps 260px apart should be thousands of raw counts apart. Anything
    // less means a bad sample got through, and a bad map is worse than none.
    if (abs(calRawX1 - calRawX0) < 500 || abs(calRawY1 - calRawY0) < 500) {
        Serial.printf("calibration rejected: dx=%d dy=%d\n",
                      calRawX1 - calRawX0, calRawY1 - calRawY0);
        touchCalibrated = false;
        return;
    }

    prefs.putInt("tx0", calRawX0);
    prefs.putInt("ty0", calRawY0);
    prefs.putInt("tx1", calRawX1);
    prefs.putInt("ty1", calRawY1);
    prefs.putInt("calver", CAL_VERSION);

    touchCalibrated = true;

    Serial.printf("calibrated x %d..%d  y %d..%d\n",
                  calRawX0, calRawX1, calRawY0, calRawY1);
}

static bool touchToScreen(const TS_Point &p, int &sx, int &sy) {

    if (!touchCalibrated) return false;

    sx = CAL_X0 + (int32_t)(p.x - calRawX0) * (CAL_X1 - CAL_X0) / (calRawX1 - calRawX0);
    sy = CAL_Y0 + (int32_t)(p.y - calRawY0) * (CAL_Y1 - CAL_Y0) / (calRawY1 - calRawY0);

    return true;
}

// Short press toggles the graph, a 3 second hold redoes the calibration.
static void checkButton() {

    static bool     wasDown   = false;
    static uint32_t downAt    = 0;
    static bool     longFired = false;

    const bool down = digitalRead(BOOT_BTN_PIN) == LOW;

    if (down && !wasDown) {
        downAt    = millis();
        longFired = false;
    }

    if (down && !longFired && millis() - downAt > 3000) {
        longFired = true;
        calibrateTouch();
        drawChrome();
        if (lastReading.valid) refresh(lastReading);
    }

    if (!down && wasDown && !longFired && millis() - downAt > 30) {
        toggleGraph();
    }

    wasDown = down;
}

// Only taps inside the button's hit box count now. Tap-anywhere is gone - with
// a real button on screen, brushing the panel while moving the device would
// otherwise toggle the graph.
static void checkTouch() {

    static bool     wasTouched = false;
    static uint32_t lastTap    = 0;

    const bool touched = ts.touched();

    if (touched && !wasTouched && millis() - lastTap > 400) {

        const TS_Point p = ts.getPoint();
        int sx, sy;

        // Raw values are logged unconditionally: without calibration there is
        // no mapped coordinate to report, and that is exactly the case that
        // needs diagnosing.
        Serial.printf("touch raw x=%d y=%d z=%d\n", p.x, p.y, p.z);

        if (touchToScreen(p, sx, sy)) {
            Serial.printf("  mapped x=%d y=%d\n", sx, sy);

            if (sx >= HIT_X0 && sx <= HIT_X1 && sy >= HIT_Y0 && sy <= HIT_Y1) {
                lastTap = millis();
                toggleGraph();
            }
        }
    }

    wasTouched = touched;
}

// Full-screen message, used for every state that is not a live reading.
static void showMessage(const char *line1, const char *line2, uint16_t colour) {

    tft.fillScreen(C_BG);
    chromeDrawn = false;

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(colour, C_BG);
    tft.drawString(line1, tft.width() / 2, tft.height() / 2 - 20, 4);

    if (line2) {
        tft.setTextColor(C_LABEL, C_BG);
        tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 20, 2);
    }
}

// ---------------------------------------------------------------- network

static int lastHttpCode = 0;

static FetchResult fetchReading(Reading &out) {

    if (WiFi.status() != WL_CONNECTED) return FETCH_NO_WIFI;

    WiFiClientSecure client;

    client.setCACertBundle(rootca_crt_bundle_start);
    client.setTimeout(10000);

    HTTPClient http;

    // The token goes in a header rather than the query string, so it does not
    // end up in Northflank's access logs. Verified against this instance:
    // api-secret works, Authorization: Bearer does not.
    String url = String("https://") + NS_HOST +
                 "/api/v1/entries.json?count=" + String(GRAPH_MAX_PTS);

    if (!http.begin(client, url)) {
        Serial.println("http.begin failed");
        return FETCH_TLS;
    }

    http.addHeader("api-secret", NS_TOKEN);

    const int code = http.GET();
    lastHttpCode = code;

    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTP %d\n", code);
        http.end();

        // Negative codes come from the transport layer, which is where a
        // failed certificate check surfaces.
        if (code < 0)                        return FETCH_TLS;
        if (code == 401 || code == 403)      return FETCH_AUTH;

        return FETCH_HTTP;
    }

    JsonDocument doc;

    // Only the fields we actually use, so the document stays small.
    JsonDocument filter;
    filter[0]["sgv"]       = true;
    filter[0]["date"]      = true;
    filter[0]["direction"] = true;

    const DeserializationError err =
        deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));

    http.end();

    if (err) {
        Serial.printf("JSON error: %s\n", err.c_str());
        return FETCH_DATA;
    }
    if (doc.isNull() || doc.size() == 0) {
        Serial.println("no entries returned");
        return FETCH_DATA;
    }

    // Entries arrive newest first. Skip any without an sgv - Nightscout mixes
    // calibration and meter records into the same collection.
    history.count = 0;

    for (JsonObject e : doc.as<JsonArray>()) {

        if (!e["sgv"].is<int>()) continue;

        if (!out.valid) {
            out.sgv       = e["sgv"].as<int>();
            out.dateMs    = e["date"].as<int64_t>();
            out.direction = e["direction"].as<const char *>() ?: "NONE";
            out.valid     = true;

            history.newestMs = out.dateMs;
        }

        if (history.count < GRAPH_MAX_PTS) {
            history.sgv[history.count++] = e["sgv"].as<int>();
        }
    }

    if (!out.valid) {
        Serial.println("no entry with an sgv");
        return FETCH_DATA;
    }

    Serial.printf("sgv=%d dir=%s history=%d\n",
                  out.sgv, out.direction.c_str(), history.count);
    return FETCH_OK;
}

static void showFetchError(FetchResult r) {

    static char detail[24];

    switch (r) {
        case FETCH_TLS:
            showMessage("Secure connect", "certificate not trusted", C_URGENT);
            break;
        case FETCH_AUTH:
            showMessage("Auth failed", "check NS_TOKEN", C_URGENT);
            break;
        case FETCH_HTTP:
            snprintf(detail, sizeof(detail), "HTTP %d", lastHttpCode);
            showMessage("Nightscout error", detail, C_URGENT);
            break;
        case FETCH_NO_WIFI:
            showMessage("No WiFi", "retrying...", C_URGENT);
            break;
        default:
            showMessage("No data", "no recent entry", C_URGENT);
            break;
    }
}

static void connectWiFi() {

    showMessage("Connecting WiFi", WIFI_SSID, C_LABEL);

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t deadline = millis() + 20000;

    while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() != WL_CONNECTED) {
        showMessage("WiFi failed", "retrying...", C_URGENT);
        return;
    }

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

// ---------------------------------------------------------------- sketch

void setup() {

    Serial.begin(115200);
    Serial.println("\nNightscout glucose display");

    pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

    prefs.begin("glucose", false);
    graphOn = prefs.getBool("graph", SHOW_GRAPH);
    Serial.printf("graph %s\n", graphOn ? "on" : "off");

    calRawX0 = prefs.getInt("tx0", 0);
    calRawY0 = prefs.getInt("ty0", 0);
    calRawX1 = prefs.getInt("tx1", 0);
    calRawY1 = prefs.getInt("ty1", 0);

    touchCalibrated = prefs.getInt("calver", 0) == CAL_VERSION &&
                      calRawX1 != calRawX0 && calRawY1 != calRawY0;

    Serial.printf("touch %s\n", touchCalibrated ? "calibrated" : "needs calibration");

    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(C_BG);

    if (!touchCalibrated) calibrateTouch();

    connectWiFi();

    showMessage("Loading...", NULL, C_LABEL);

    const FetchResult r = fetchReading(lastReading);

    if (r == FETCH_OK) refresh(lastReading);
    else               showFetchError(r);

    lastPollMs = millis();
}

void loop() {

    if (millis() - lastPollMs < (uint32_t)POLL_SECONDS * 1000) {

        // Between polls: keep the age line honest, drive the urgent blink, and
        // stay responsive to the button. refresh() returns early when nothing
        // has actually changed, so polling it this often is cheap.
        checkButton();
        checkTouch();
        if (lastReading.valid) refresh(lastReading);

        delay(20);
        return;
    }

    lastPollMs = millis();

    if (WiFi.status() != WL_CONNECTED) {
        connectWiFi();
        return;
    }

    Reading fresh;

    const FetchResult r = fetchReading(fresh);

    if (r == FETCH_OK) {
        lastReading = fresh;
        refresh(lastReading);
        return;
    }

    // A transient failure leaves a recent reading on screen - the age counter
    // is the honest signal there. But once that reading is too old to act on,
    // stop showing a number at all and say what is actually wrong instead.
    if (!lastReading.valid || minutesAgo(lastReading.dateMs) > 15) {
        showFetchError(r);
    }
}
