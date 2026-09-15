// NitroWorks ECU - OLED pairing-screen mockup browser
//
// Renders each pairing OLED screen from ECU-SPEC-002-oled-screens.md (rev 3) as a
// self-contained animated mockup. One button on GPIO15 steps through them.
// Boots on mockup 1 (S1 Search). All screens are GFX primitives + the built-in
// 5x7 font at setTextSize(1)/(2), plus (3) for the S1/S2 word and S0's "LOW".
// No stored bitmaps. Panel: 1.3" SH1106, 128x64, driven by Adafruit_SH1106G
// (not SSD1306). The SH1106 has a 132px internal map / 2px offset, so every
// layout keeps a 2px edge margin.
//
// S0 Battery Critical is a global, preemptive firmware state (powerState ==
// CRITICAL): it also cuts drive, tears down Bluetooth, and turns the LED ring
// off, with the buzzer double-chirping. None of that is modelled here - this is
// a screen browser, so S0 is just one more entry in the list.
//
// S4 is the post-connect stick check: one axis at a time (idle glyph, then
// ACC / REV / TURN L / TURN R once you push the stick). With no real joystick
// here, mockup 6 cycles those states on a ~1.6 s timer. The idle glyph is the
// gamepad silhouette adopted in ECU-SPEC-002 rev 3 (S4_IDLE_GLYPH 1);
// S4_IDLE_GLYPH 2 keeps the rejected "knob + 4 arrows" alternative for reference.

#include <Arduino.h>
#include <Wire.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C  // some 1.3" modules use 0x3D
#define W SH110X_WHITE

// S4 idle glyph.
//   1 = gamepad silhouette  (ECU-SPEC-002 rev 3 - the adopted design)
//   2 = centre knob + 4 arrows  (rejected alternative, kept for reference)
#define S4_IDLE_GLYPH 1

constexpr int PIN_NAV_BTN = 15;          // nav button: GPIO15 -> button -> GND
constexpr uint32_t BTN_DEBOUNCE_MS = 40;
constexpr uint32_t FRAME_MS = 160;       // ~6 Hz refresh, per the spec

Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---- battery status strip states --------------------------------------------
// Only two: normal and warning. Critical is NOT a strip state - it takes the
// whole screen (S0), see renderBatteryCritical().
enum BattState { BATT_NORMAL, BATT_WARNING };

// ---- mockup list -----------------------------------------------------------
const char* const MOCKUP_NAMES[] = {
  "1  S1  Search state - OLED word 'SCAN' (82%)",
  "2  S1b Search - low battery warning (~15%)",
  "3  S2  Pair (69%)",
  "4  S2b Pair - low battery warning (~15%)",
  "5  S3  Connected (READY)",
  "6  S4  Stick check (cycles idle / ACC / REV / TURN L / TURN R)",
  "7  S0  Battery Critical (global lockout)",
};
constexpr int NUM_MOCKUPS = sizeof(MOCKUP_NAMES) / sizeof(MOCKUP_NAMES[0]);

int screen = 0;  // current mockup index, 0-based (boots on mockup 1)

// ---- debounced active-low button ------------------------------------------
struct Button {
  int pin;
  int stable;
  int lastReading;
  uint32_t lastChangeMs;
};
Button navBtn = {PIN_NAV_BTN, HIGH, HIGH, 0};

void initButton(Button& b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.stable = b.lastReading = digitalRead(b.pin);
  b.lastChangeMs = millis();
}

// Returns true exactly once per press (debounced HIGH -> LOW edge).
bool buttonPressed(Button& b) {
  int reading = digitalRead(b.pin);
  if (reading != b.lastReading) {
    b.lastReading = reading;
    b.lastChangeMs = millis();
  }
  if (millis() - b.lastChangeMs >= BTN_DEBOUNCE_MS && reading != b.stable) {
    b.stable = reading;
    if (b.stable == LOW) return true;
  }
  return false;
}

// ---- shared animation clocks (time-based, per the "Draw calls" appendix) ---
int animWaves() { return (millis() / 300) % 4; }        // searching arcs: 0,1,2,3 -> none/w1/+w2/+w3, repeat
bool animBlink() { return (millis() / 480) % 2 == 0; }  // "toggle every 3 frames"

// ---- draw helpers --------------------------------------------------------

// The Bluetooth glyph: 5 drawLine segments (stem, two shoulders, two crossing
// diagonals), centred at (cx,cy) with half-height h and half-width wd. Drawn
// twice at a 1px offset when bold (S3 variant); optional filled "connected" dot.
void drawBtGlyph(int cx, int cy, int h, int wd, bool bold, bool dot) {
  for (int o = 0; o < (bold ? 2 : 1); o++) {
    int d = o;  // 1px x-offset on the bold pass
    display.drawLine(cx + d, cy - h, cx + d, cy + h, W);            // stem
    display.drawLine(cx + d, cy - h, cx + wd + d, cy - h / 2, W);   // upper shoulder
    display.drawLine(cx + d, cy + h, cx + wd + d, cy + h / 2, W);   // lower shoulder
    display.drawLine(cx + wd + d, cy - h / 2, cx - wd + d, cy + h / 2, W);  // diagonal
    display.drawLine(cx + wd + d, cy + h / 2, cx - wd + d, cy - h / 2, W);  // diagonal
  }
  if (dot) display.fillCircle(cx, cy, 2, W);
}

// Right-opening "searching" arcs at r = step / 2*step / 3*step about a centre
// just right of the glyph. n arcs drawn; double-drawn (r and r+1) when solid (S3).
void drawWaves(int cx, int cy, int n, bool solid, int step) {
  for (int i = 1; i <= 3 && i <= n; i++) {
    int r = i * step;
    display.drawCircleHelper(cx, cy, r, 0x6, W);  // 0x6 = right half
    if (solid) display.drawCircleHelper(cx, cy, r + 1, 0x6, W);
  }
}

// One shared status-strip routine for S1 / S2 / S4. Ends by drawing the
// full-width divider at y = 20 (2px edge margin -> x 2..125).
void drawStatusStrip(BattState state, int pct, bool hudBtIcon) {
  display.setTextSize(2);
  if (state == BATT_NORMAL) {
    display.drawRect(2, 4, 32, 14, W);            // battery body
    display.fillRect(34, 8, 3, 6, W);             // nub
    int fw = (int)((pct / 100.0f) * 28);
    display.fillRect(4, 6, fw, 10, W);            // proportional charge
    display.setCursor(44, 3);
    display.print(pct);
    display.print("%");
  } else {  // BATT_WARNING
    display.drawRect(2, 3, 40, 16, W);            // bigger empty outline (solid)
    display.fillRect(42, 8, 3, 6, W);             // nub
    if (animBlink()) {                            // exclamation, centred in the outline
      display.fillRect(20, 6, 3, 6, W);           //   stem  (interior centre ~x21.5 / y10.5)
      display.fillRect(20, 14, 3, 2, W);          //   dot   (2px margin top & bottom)
    }
    display.setCursor(48, 3);
    display.print("15%");                         // constant
  }

  if (hudBtIcon) drawBtGlyph(115, 10, 9, 6, true, true);  // connected icon (glyph + dot), right edge

  display.drawLine(2, 20, 125, 20, W);           // divider
}

// State word: static, size 3, centred in the gap between the left screen edge and
// the Bluetooth glyph (which starts at x~80), vertically centred in Area 2.
// No trailing dots - the searching waves carry the motion.
// ("SCAN" / "PAIR" are 4 chars -> 69 px wide at size 3; cursor x6 -> ends x75.)
void drawWord(const char* word) {
  display.setTextSize(3);
  display.setCursor(6, 31);
  display.print(word);
}

// ---- individual screens -------------------------------------------------

// S1 / S1b / S2 / S2b share this: only word + strip state + pct differ
// (ECU-SPEC-002 describes S1 and S2 as the same screen).
void renderSearchLike(const char* word, BattState batt, int pct) {
  drawStatusStrip(batt, pct, false);
  drawWord(word);                                  // static, size 3, x6
  drawBtGlyph(97, 41, 17, 8, false, false);        // pushed right (leftmost ~x89) -> ~15px gap
                                                   //   after the word, r 5/10/15 waves reach ~x123
  drawWaves(108, 41, animWaves(), false, 5);       //   (~2px inside the x125 safety margin)
}

// S3 Connected: READY + the "connected" counterpart of the searching mark.
// Glyph + waves are the same size as S1 / S2 (half-h 17, r 5/10/15); bold, all
// three waves solid, a dot centred on the glyph.
void renderConnected() {
  display.setTextSize(2);
  display.setCursor(34, 6);
  display.print("READY");
  drawBtGlyph(55, 40, 17, 8, true, true);         // dot centred at (55,40)
  drawWaves(66, 40, 3, true, 5);
}

// small solid triangle, w wide / h tall, apex at the right (pointRight) or left.
void fillTri(int x, int y, int w, int h, bool pointRight) {
  for (int i = 0; i < w; i++) {
    int t  = pointRight ? (w - 1 - i) : i;          // tall end
    int hh = 1 + (h - 1) * t / (w - 1);
    display.drawFastVLine(x + i, y + (h - hh) / 2, hh, W);
  }
}

// S4 Stick-check HUD: one axis at a time. No real joystick here, so this cycles
// idle -> ACC -> REV -> TURN R -> TURN L (~1.6 s each); the active bar breathes
// from a synthetic sine so the fill motion reads. Idle glyph is selected by
// S4_IDLE_GLYPH (1 = adopted gamepad silhouette, 2 = rejected knob + 4 arrows).
void renderStickCheck() {
  drawStatusStrip(BATT_NORMAL, 82, true);

  int   st = (int)((millis() / 1600) % 5);          // 0 idle 1 ACC 2 REV 3 TURN R 4 TURN L
  float m  = sinf(millis() / 500.0f) * 0.5f + 0.5f; // synthetic magnitude 0..1

  if (st == 0) {                                    // idle glyph (see S4_IDLE_GLYPH)
#if S4_IDLE_GLYPH == 1
    display.drawRoundRect(40, 32, 48, 20, 8, W);            // gamepad body
    display.fillRect(48, 37, 4, 10, W);                     // d-pad "+", left-justified,
    display.fillRect(45, 40, 10, 4, W);                     //   centred on the body midline
    display.fillCircle(80, 38, 2, W);                       // face buttons (vertical pair)
    display.fillCircle(80, 46, 2, W);
#else
    display.fillCircle(64, 40, 3, W);                       // centre knob
    display.fillTriangle(64, 25, 57, 32, 71, 32, W);        // up
    display.fillTriangle(64, 55, 57, 48, 71, 48, W);        // down
    display.fillTriangle(46, 40, 53, 33, 53, 47, W);        // left
    display.fillTriangle(82, 40, 75, 33, 75, 47, W);        // right
#endif
    return;
  }

  const int BX = 4, BY = 40, BW = 120, BH = 16;
  const char* label = (st == 1) ? "ACC" : (st == 2) ? "REV"
                    : (st == 3) ? "TURN R" : "TURN L";

  display.setTextSize(2);
  display.setCursor(4, 23);
  display.print(label);
  if (st == 1) fillTri(4 + (int)strlen(label) * 12 + 2, 25, 7, 9, true);
  if (st == 2) fillTri(4 + (int)strlen(label) * 12 + 2, 25, 7, 9, false);

  char val[6];
  snprintf(val, sizeof(val), "%d%%", (int)(m * 100));
  display.setCursor(124 - (int)strlen(val) * 12, 23);
  display.print(val);

  display.drawRect(BX, BY, BW, BH, W);
  if (st == 1) {                                    // ACC - anchored left
    display.fillRect(BX + 2, BY + 2, (int)((BW - 4) * m), BH - 4, W);
  } else if (st == 2) {                             // REV - anchored right
    int w = (int)((BW - 4) * m);
    display.fillRect(BX + BW - 2 - w, BY + 2, w, BH - 4, W);
  } else {                                          // TURN - centre-out from the zero tick
    int cx = BX + BW / 2;
    display.drawFastVLine(cx, BY - 2, BH + 4, W);
    int w = (int)((BW / 2 - 4) * m);
    if (st == 3) display.fillRect(cx + 2, BY + 2, w, BH - 4, W);
    else         display.fillRect(cx - 2 - w, BY + 2, w, BH - 4, W);
  }
}

// S0 Battery Critical - large empty battery + centred "!" (blink together),
// with a single size-3 word "LOW" that stays lit. No strip, no BT glyph, no %.
void renderBatteryCritical() {
  if (animBlink()) {
    display.drawRect(22, 3, 84, 26, W);           // large empty battery outline
    display.fillRect(106, 11, 4, 10, W);          // terminal nub
    display.fillRect(61, 7, 4, 13, W);            // "!" stem
    display.fillRect(61, 23, 4, 4, W);            // "!" dot
  }
  display.setTextSize(3);
  display.setCursor(37, 36);
  display.print("LOW");                           // constant
}

void renderScreen() {
  display.clearDisplay();
  switch (screen) {
    case 0: renderSearchLike("SCAN", BATT_NORMAL,  82); break;
    case 1: renderSearchLike("SCAN", BATT_WARNING, 15); break;
    case 2: renderSearchLike("PAIR",   BATT_NORMAL,  69); break;
    case 3: renderSearchLike("PAIR",   BATT_WARNING, 15); break;
    case 4: renderConnected();       break;
    case 5: renderStickCheck();      break;
    case 6: renderBatteryCritical(); break;
  }
  display.display();
}

void printLegend() {
  Serial.println();
  Serial.println("NitroWorks ECU - OLED pairing-screen mockup browser");
  Serial.println("Press the GPIO15 button to step through:");
  for (int i = 0; i < NUM_MOCKUPS; i++) {
    Serial.print("  ");
    Serial.println(MOCKUP_NAMES[i]);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  initButton(navBtn);

  Wire.begin();  // ESP32 default: SDA=21, SCL=22
  if (!display.begin(OLED_ADDR, true)) {
    Serial.println("SH1106 begin failed - check wiring / try address 0x3D");
    while (true) {
      delay(1000);
    }
  }
  display.setTextColor(W);

  printLegend();
  Serial.printf("Showing mockup %s\n", MOCKUP_NAMES[screen]);
}

void loop() {
  if (buttonPressed(navBtn)) {
    screen = (screen + 1) % NUM_MOCKUPS;
    Serial.printf("Showing mockup %s\n", MOCKUP_NAMES[screen]);
  }
  renderScreen();
  delay(FRAME_MS);
}
