#include "oled_ui.h"

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>

#include "config.h"
#include "robot_state.h"
#include "uart_link.h"

Adafruit_SH1106G display(128, 64, &Wire, -1);
bool g_oledOk = false;

// The OLED draw helpers below are a VERBATIM port of
// 06_Code_Sprints/BluetoothUIMockup/src/main.cpp - the pixel-accurate reference
// for ECU-SPEC-002, tuned on the real 1.3" SH1106. Do NOT re-derive them from
// the spec text; if a screen changes, change it in the mockup first, then copy
// it here. DualCore only supplies the live inputs: the per-frame RobotState
// snapshot's linkState picks the screen and the real gamepad sticks (throttle /
// steer) drive S4 via s4PickAxis().
#define W SH110X_WHITE

// Only two strip states: normal and warning. Critical takes the whole screen
// (S0) - step 5. Step 3 only ever passes BATT_NORMAL.
enum BattState { BATT_NORMAL, BATT_WARNING };

// ---- shared animation clocks (mockup) ------------------------------------
int  animWaves() { return (millis() / 300) % 4; }        // searching arcs: 0,1,2,3
bool animBlink() { return (millis() / 480) % 2 == 0; }   // "toggle every 3 frames"

// ---- draw helpers (verbatim from BluetoothUIMockup) --------------------------

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
      display.fillRect(20, 6, 3, 6, W);           //   stem
      display.fillRect(20, 14, 3, 2, W);          //   dot
    }
    display.setCursor(48, 3);
    display.print("15%");                         // constant
  }

  if (hudBtIcon) drawBtGlyph(115, 10, 9, 6, true, true);  // connected icon (glyph + dot), right edge

  display.drawLine(2, 20, 125, 20, W);           // divider
}

// State word: static, size 3, x6, vertically centred in Area 2.
void drawWord(const char* word) {
  display.setTextSize(3);
  display.setCursor(6, 31);
  display.print(word);
}

// S1 / S1b / S2 / S2b share this: only word + strip state + pct differ.
void renderSearchLike(const char* word, BattState batt, int pct) {
  drawStatusStrip(batt, pct, false);
  drawWord(word);                                  // static, size 3, x6
  drawBtGlyph(97, 41, 17, 8, false, false);        // pushed right (leftmost ~x89)
  drawWaves(108, 41, animWaves(), false, 5);       // r 5/10/15 waves reach ~x123
}

// S3 Connected: READY + the "connected" counterpart of the searching mark.
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

// ---- S4 axis selection - DualCore-specific (real sticks, not the mockup timer) --
// One axis owns Area 2; ~400 ms linger back to Idle (ECU-SPEC-002 §4).
enum S4Axis : uint8_t { S4_IDLE, S4_ACC, S4_REV, S4_TURN_L, S4_TURN_R };

S4Axis s4PickAxis(int32_t thr, int32_t str) {
  static S4Axis   held     = S4_IDLE;
  static uint32_t lingerAt = 0;

  bool thrActive = abs(thr) >= S4_DEADZONE;
  bool strActive = abs(str) >= S4_DEADZONE;

  S4Axis want = held;
  if (thrActive || strActive) {
    // Both crossing together from Idle: throttle wins (§4).
    if (thrActive && (!strActive || held == S4_ACC || held == S4_REV || held == S4_IDLE))
      want = (thr > 0) ? S4_ACC : S4_REV;
    else
      want = (str > 0) ? S4_TURN_R : S4_TURN_L;
    lingerAt = 0;
  } else if (held != S4_IDLE) {
    if (lingerAt == 0) lingerAt = millis();
    if (millis() - lingerAt >= S4_LINGER_MS) { want = S4_IDLE; lingerAt = 0; }
  }
  held = want;
  return held;
}

// S4 Stick-check HUD - mockup geometry (BX/BY/BW/BH, label + fillTri arrow,
// right-aligned value, ACC-left / REV-right / TURN centre-out, idle gamepad
// glyph). The state comes from s4PickAxis() and the magnitude `m` from the live
// axis rescaled past the deadzone, replacing the mockup's synthetic sine.
// thr/str come from this frame's RobotState snapshot (see uiTask).
void drawS4(int32_t thr, int32_t str) {
  drawStatusStrip(BATT_NORMAL, BATT_PCT_STUB, true);
  S4Axis axis = s4PickAxis(thr, str);

  if (axis == S4_IDLE) {                                    // idle gamepad glyph
    display.drawRoundRect(40, 32, 48, 20, 8, W);            // gamepad body
    display.fillRect(48, 37, 4, 10, W);                     // d-pad "+", left-justified,
    display.fillRect(45, 40, 10, 4, W);                     //   centred on the body midline
    display.fillCircle(80, 38, 2, W);                       // face buttons (vertical pair)
    display.fillCircle(80, 46, 2, W);
    return;
  }

  const int BX = 4, BY = 40, BW = 120, BH = 16;
  int32_t raw = (axis == S4_ACC || axis == S4_REV) ? thr : str;
  float m = (float)(abs(raw) - S4_DEADZONE) / (float)(AXIS_MAX - S4_DEADZONE);
  m = constrain(m, 0.0f, 1.0f);

  const char* label = (axis == S4_ACC) ? "ACC" : (axis == S4_REV) ? "REV"
                    : (axis == S4_TURN_R) ? "TURN R" : "TURN L";

  display.setTextSize(2);
  display.setCursor(4, 23);
  display.print(label);
  if (axis == S4_ACC) fillTri(4 + (int)strlen(label) * 12 + 2, 25, 7, 9, true);
  if (axis == S4_REV) fillTri(4 + (int)strlen(label) * 12 + 2, 25, 7, 9, false);

  char val[6];
  snprintf(val, sizeof(val), "%d%%", (int)(m * 100));
  display.setCursor(124 - (int)strlen(val) * 12, 23);
  display.print(val);

  display.drawRect(BX, BY, BW, BH, W);
  if (axis == S4_ACC) {                             // ACC - anchored left
    display.fillRect(BX + 2, BY + 2, (int)((BW - 4) * m), BH - 4, W);
  } else if (axis == S4_REV) {                      // REV - anchored right
    int w = (int)((BW - 4) * m);
    display.fillRect(BX + BW - 2 - w, BY + 2, w, BH - 4, W);
  } else {                                          // TURN - centre-out from the zero tick
    int cx = BX + BW / 2;
    display.drawFastVLine(cx, BY - 2, BH + 4, W);
    int w = (int)((BW / 2 - 4) * m);
    if (axis == S4_TURN_R) display.fillRect(cx + 2, BY + 2, w, BH - 4, W);
    else                   display.fillRect(cx - 2 - w, BY + 2, w, BH - 4, W);
  }
}

// S5 Reset toast (ECU-SPEC-002 §4) - no mockup entry; growing dots per §3.3.
void drawResetToast() {
  int dots = (millis() / 300) % 3 + 1;
  char buf[10] = "RESET";
  for (int i = 0; i < dots; i++) strcat(buf, ".");
  display.setTextSize(2);
  display.setCursor(18, 24);
  display.print(buf);
}

void uiTask(void*) {
  Serial.printf("ui  started on core %d\n", xPortGetCoreID());
  logStack("ui ");

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  g_oledOk = display.begin(0x3C, true);
  Serial.printf("ui  SH1106 begin: %s\n", g_oledOk ? "ok" : "FAILED (check wiring/addr)");
  if (g_oledOk) {
    display.setTextColor(SH110X_WHITE);
    display.clearDisplay();
    display.display();
  }

  LinkState lastEmitted = (LinkState)255;   // force a first emit
  uint32_t frames = 0;

  for (;;) {
    // One locked copy for the whole frame - every field below comes from the
    // same btTask iteration (see publishState()/readState()).
    RobotState snap = readState();

    // ---- pick the screen (ECU-SPEC-002 §5), powerState omitted (step 5) ----
    LinkState link   = snap.linkState;
    bool connected   = snap.connected;
    uint32_t connAt  = snap.connectedAtMs;
    uint32_t resetAt = snap.resetToastAtMs;
    uint32_t pairAt  = snap.pairBlankAtMs;
    uint32_t now     = millis();

    bool showReset = resetAt != 0 && (now - resetAt) < RESET_TOAST_MS;
    bool pairBlank = pairAt  != 0 && (now - pairAt)  < PAIR_BLANK_MS;

    // ---- RING / BUZZ emit on link-state change (this sprint: uiTask owns TX) --
    if (link != lastEmitted) {
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "RING %s", ringCmdFor(link));
      emitCmd(cmd);
      if (link == LINK_CONNECTED) emitCmd("BUZZ CONNECT");
      lastEmitted = link;
    }
    pumpUart1Rx();

    // ---- render ----
    if (g_oledOk) {
      display.clearDisplay();
      if (showReset) {
        drawResetToast();
      } else if (link == LINK_CONNECTED) {
        if (now - connAt < S3_HOLD_MS) renderConnected();                    // S3
        else                           drawS4(snap.throttle, snap.steer);    // S4 stick-check HUD
      } else if (link == LINK_PAIR) {
        if (pairBlank)
          drawStatusStrip(BATT_NORMAL, BATT_PCT_STUB, false);   // header only - Pair-press feedback
        else
          renderSearchLike("PAIR", BATT_NORMAL, BATT_PCT_STUB); // S2
      } else {
        renderSearchLike("SCAN", BATT_NORMAL, BATT_PCT_STUB);   // S1
      }
      display.display();
    }
    (void)connected;

    if ((frames % 30) == 29) logStack("ui ");

    frames++;
    vTaskDelay(pdMS_TO_TICKS(UI_FRAME_MS));
  }
}
