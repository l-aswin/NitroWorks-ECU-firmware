#include <Arduino.h>
#include <Preferences.h>
#include<Bluepad32.h>

constexpr int PIN_MODE_LED = 2;        // status LED: 3 states (see loop())
constexpr int PIN_COMMS_LED = 4;  // comms LED: flashes when controller input changes
constexpr int PIN_PAIR_BTN = 15;  // Pair button, active-low with internal pull-up
constexpr int PIN_RESET_BTN = 13;  // Reset button (resets the controller bond), active-low with internal pull-up

constexpr uint32_t COMMS_BLINK_MS = 30;  // comms LED on-time per input change
constexpr int32_t STICK_DEADZONE = 24;   // ignore idle axis jitter / rest offsets

// Pressing Pair enters Pairing mode: the active controller is disconnected and
// the ECU accepts one new controller. Pairing mode stays open until a controller
// connects (no timeout). A fresh (unbonded) unit boots straight into Pairing mode.
constexpr uint32_t BTN_DEBOUNCE_MS = 40;

// NVS: which controller (if any) this car is bonded to. Bluepad32 4.1.0 exposes
// no bond-list query, so we track it ourselves. ECU-SPEC-001 §8.
constexpr char NVS_NS[] = "nitro-ecu";
constexpr char NVS_KEY_BONDED[] = "bonded";

ControllerPtr controllers[BP32_MAX_GAMEPADS];

Preferences prefs;
bool hasBondedController = false;      // mirror of the NVS flag, loaded in setup()
bool pairingMode = false;             // true = Pairing mode (fast blink)
uint32_t commsBlinkOffAt = 0;        // 0 = comms LED idle

// Debounced active-low button (button shorts the pin to GND; internal pull-up).
struct Button {
  int pin;
  int stable;                        // debounced level (HIGH = released)
  int lastReading;
  uint32_t lastChangeMs;
};

Button pairBtn = {PIN_PAIR_BTN, HIGH, HIGH, 0};
Button resetBtn = {PIN_RESET_BTN, HIGH, HIGH, 0};

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

bool pairingWindowOpen() {
  return pairingMode;
}

void persistBonded(bool v) {
  size_t n = prefs.putBool(NVS_KEY_BONDED, v);
  hasBondedController = v;
  Serial.printf("persistBonded(%d) -> %u bytes written\n", v, (unsigned)n);
}

bool anyControllerConnected() {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] && controllers[i]->isConnected()) return true;
  }
  return false;
}

void openPairingWindow() {
  // Disconnect the active controller and delete the stored key, then open up for
  // one new controller. forgetBluetoothKeys() is all-or-nothing, so it has to
  // happen here while the old key is the only one - deleting it later would wipe
  // the new controller's key too. hasBondedController is left as-is: it's only
  // cleared once a new controller actually bonds (see onConnectedController), so
  // an abandoned attempt still boots back into normal mode.
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i]) {
      controllers[i]->disconnect();
      controllers[i] = nullptr;
    }
  }
  digitalWrite(PIN_MODE_LED, LOW);
  BP32.forgetBluetoothKeys();
  BP32.enableNewBluetoothConnections(true);
  pairingMode = true;
  Serial.println("Pairing mode - disconnected the active controller. Put the new "
                 "controller in pairing mode (Blitz: hold Power + A).");
}

void closePairingWindow(const char* why) {
  BP32.enableNewBluetoothConnections(false);
  pairingMode = false;
  Serial.printf("Pairing mode ended: %s\n", why);
}

// Reset: clear the stored bond and drop straight into pairing mode, ready to
// bond a new controller. The NVS write happens FIRST, before any BP32 call, so
// the flag is durably cleared even if forgetBluetoothKeys() blocks or resets the
// stack partway through.
void resetBondedController() {
  Serial.println("Reset button - clearing stored bond");
  persistBonded(false);
  openPairingWindow();  // disconnects, BP32.forgetBluetoothKeys(), pairing mode on
}

void pollButtons() {
  if (buttonPressed(pairBtn)) {
    Serial.println("Pair button pressed");
    openPairingWindow();   // a press during an open window just restarts it
  }
  if (buttonPressed(resetBtn)) {
    resetBondedController();
  }
}

void onConnectedController(ControllerPtr ctl) {
  bool bondedBefore = hasBondedController;
  bool foundSlot = false;
  int slot = -1;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] == nullptr) {
      controllers[i] = ctl;
      slot = i;
      foundSlot = true;
      break;
    }
  }
  if (!foundSlot) {
    Serial.println("Controller connected but no empty slot");
    return;
  }

  // Guard against the known Bluepad32 caveat where enableNewBluetoothConnections(false)
  // does not always reject unbonded controllers on the BT-Classic path: a connect
  // with no bond and no open pairing window should be impossible - drop it.
  if (!bondedBefore && !pairingWindowOpen()) {
    Serial.println("Rejecting unexpected controller (no bond, not pairing)");
    controllers[slot] = nullptr;
    ctl->disconnect();
    return;
  }

  Serial.printf("Controller connected, slot %d\n", slot);
  if (pairingWindowOpen()) {
    // This is the new controller the player is switching to. openPairingWindow()
    // already dropped the old one; clear any other slot defensively (e.g. a stale
    // reconnect that slipped in during the window) so only this one remains.
    for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
      if (i != slot && controllers[i]) {
        Serial.printf("Dropping stray controller in slot %d\n", i);
        controllers[i]->disconnect();
        controllers[i] = nullptr;
      }
    }
    persistBonded(true);  // authoritative-connect rule, ECU-SPEC-001 §8
    Serial.println("Bonded to this controller");
    closePairingWindow("new controller bonded");
  }
  // Window closed => the guard above guarantees this is the bonded controller
  // reconnecting; nothing to persist.
  digitalWrite(PIN_MODE_LED, HIGH);
}

void onDisconnectedController(ControllerPtr ctl) {
  // A disconnect is not a reset - leave the bonded flag alone.
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] == ctl) {
      Serial.printf("Controller disconnected, slot %d\n", i);
      controllers[i] = nullptr;
      digitalWrite(PIN_MODE_LED, LOW);
      break;
    }
  }
}

void dumpGamepad(ControllerPtr ctl) {
  Serial.printf(
    "axes: LX=%4d LY=%4d RX=%4d RY=%4d | brake=%4d throttle=%4d | "
    "buttons=0x%04x dpad=0x%02x\n",
    ctl->axisX(), ctl->axisY(), ctl->axisRX(), ctl->axisRY(),
    ctl->brake(), ctl->throttle(),
    ctl->buttons(), ctl->dpad()
  );
}

// True while any control is off its neutral position: a stick/trigger past the
// deadzone, or any button / d-pad pressed. Idle sticks (including the ~-4 LY/RY
// zero offset) stay under the deadzone and read as inactive.
bool inputActive(ControllerPtr ctl) {
  if (ctl->buttons() || ctl->miscButtons() || ctl->dpad()) return true;
  return abs(ctl->axisX())  > STICK_DEADZONE ||
         abs(ctl->axisY())  > STICK_DEADZONE ||
         abs(ctl->axisRX()) > STICK_DEADZONE ||
         abs(ctl->axisRY()) > STICK_DEADZONE ||
         abs(ctl->brake())    > STICK_DEADZONE ||
         abs(ctl->throttle()) > STICK_DEADZONE;
}

void setup() {
  pinMode(PIN_MODE_LED, OUTPUT);
  digitalWrite(PIN_MODE_LED, LOW);
  pinMode(PIN_COMMS_LED, OUTPUT);
  digitalWrite(PIN_COMMS_LED, LOW);
  initButton(pairBtn);
  initButton(resetBtn);

  Serial.begin(115200);

  prefs.begin(NVS_NS, false);
#ifdef NITRO_QA_RESET_NVS
  prefs.clear();  // QA build only: return the unit to Unpaired on boot (ECU-SPEC-001 §8)
  Serial.println("NITRO_QA_RESET_NVS: cleared stored bond");
#endif
  hasBondedController = prefs.getBool(NVS_KEY_BONDED, false);
  Serial.printf("Boot: NVS bonded flag = %d\n", hasBondedController);

  // Bring the BT stack up first so the ESP32 is connectable as early as
  // possible after boot. A gamepad that was already on only retries its lost
  // link for a short window; the sooner we are page-scanning, the more likely
  // we catch that window and auto-reconnect without user action.
  // NOTE: no unconditional BP32.forgetBluetoothKeys() here on purpose. A bonded
  // unit must keep its key across a power cycle so the controller reconnects on
  // its own. The key is cleared only inside openPairingWindow() - reached via the
  // Pair or Reset button, or automatically below when the unit has no bond yet.
  BP32.setup(&onConnectedController, &onDisconnectedController);
  BP32.enableVirtualDevice(false);

  Serial.println("Stage 2 smoke test: Bluepad32 pairing + Pair button");
  if (hasBondedController) {
    // Normal mode: reconnect to the bonded controller only, ignore everyone else.
    // The Pair button opens pairing mode to accept a different controller.
    BP32.enableNewBluetoothConnections(false);
    Serial.println("Booted bonded - reconnecting to the stored controller. "
                   "Press Pair to bond a different one.");
  } else {
    // No bond yet - go straight to pairing mode so the player doesn't have to
    // press Pair on a fresh unit.
    Serial.println("Booted Unpaired - entering pairing mode automatically.");
    openPairingWindow();
  }
}

void loop() {
  pollButtons();

  bool dataUpdated = BP32.update();
  if (dataUpdated) {
    for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
      ControllerPtr ctl = controllers[i];
      if (ctl && ctl->isConnected() && ctl->hasData() && inputActive(ctl)) {
        digitalWrite(PIN_COMMS_LED, HIGH);
        commsBlinkOffAt = millis() + COMMS_BLINK_MS;
        dumpGamepad(ctl);
      }
    }
  }
  if (commsBlinkOffAt != 0 && (int32_t)(millis() - commsBlinkOffAt) >= 0) {
    digitalWrite(PIN_COMMS_LED, LOW);
    commsBlinkOffAt = 0;
  }

  // LED status (PIN_MODE_LED), 3 states:
  //   Connected    -> solid HIGH (set in the connect handler)
  //   Pairing mode -> fast blink (125 ms)
  //   Normal mode  -> slow blink (1000 ms), searching for the paired controller
  if (pairingMode) {
    digitalWrite(PIN_MODE_LED, (millis() / 125) % 2);
  } else if (!anyControllerConnected()) {
    digitalWrite(PIN_MODE_LED, (millis() / 1000) % 2);
  }

  delay(50);  // ~20 Hz print rate, readable in the monitor
}
