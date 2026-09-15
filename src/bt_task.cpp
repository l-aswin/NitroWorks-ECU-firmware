#include "bt_task.h"

#include <Bluepad32.h>

#include "button.h"
#include "config.h"
#include "robot_state.h"

// ---- Bluepad32 + pairing (core 0) ------------------------------------------
ControllerPtr controllers[BP32_MAX_GAMEPADS];

Preferences prefs;
bool     hasBondedController = false;   // mirror of the NVS flag, loaded in setup()
bool     pairingMode         = false;   // true = pairing window open (accept any controller)
uint32_t g_pairSetupPendingAt = 0;     // core-0 only: when to run the deferred forget-keys / enable-discovery

Button pairBtn  = {PIN_PAIR_BTN,  HIGH, HIGH, 0};
Button resetBtn = {PIN_RESET_BTN, HIGH, HIGH, 0};

bool pairingWindowOpen() { return pairingMode; }

void persistBonded(bool v) {
  size_t n = prefs.putBool(NVS_KEY_BONDED, v);
  hasBondedController = v;
  g_pending.hasBond = v;
  Serial.printf("persistBonded(%d) -> %u bytes written\n", v, (unsigned)n);
}

bool anyControllerConnected() {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] && controllers[i]->isConnected()) return true;
  }
  return false;
}

void openPairingWindow() {
  // Flip the pending state FIRST so it wins over computeLinkState()'s result
  // later this same btTask iteration - both land in g_state together at the
  // single publishState() call at the loop tail, so core 1 still sees PAIR on
  // its very next frame; nothing here waits for computeLinkState().
  pairingMode = true;
  g_pending.linkState = LINK_PAIR;

  // Disconnect the active controller and (soon) forget the stored key, then open
  // up for one new controller. forgetBluetoothKeys() is all-or-nothing, so it
  // has to happen while the old key is the only one. hasBondedController is left
  // as-is - only cleared once a new controller actually bonds - so an abandoned
  // attempt still boots back into Search.
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i]) {
      controllers[i]->disconnect();
      controllers[i] = nullptr;
    }
  }
  digitalWrite(PIN_MODE_LED, LOW);

  // Defer forgetBluetoothKeys() + enableNewBluetoothConnections(true): both post
  // to the BT thread, whose NVS key-wipe (flash-sector erases) disables the
  // flash cache and freezes uiTask (GFX runs from flash, no PSRAM) for a few
  // hundred ms. Running them PAIR_SETUP_DEFER_MS later lets the S2 feedback
  // frame reach the panel first (see btTask). A real controller takes seconds
  // to pair, so entering discovery ~200 ms late is invisible.
  g_pairSetupPendingAt = millis();
  Serial.println("Pairing window open - waiting for a new controller.");
}

void closePairingWindow(const char* why) {
  BP32.enableNewBluetoothConnections(false);
  pairingMode = false;
  Serial.printf("Pairing window closed: %s\n", why);
}

// Reset: clear the stored bond FIRST (durable even if a later BP32 call blocks),
// then drop into an open pairing window. Also fires the S5 toast on core 1.
void resetBondedController() {
  Serial.println("Reset button - clearing stored bond");
  persistBonded(false);
  g_pending.resetToastAtMs = millis();
  openPairingWindow();
}

void pollButtons() {
  if (buttonPressed(pairBtn)) {
    Serial.println("Pair button pressed");
    // Intentional press feedback: blank everything below the header for
    // PAIR_BLANK_MS. Only when entering PAIR from SCAN / CONNECTED - a re-press
    // while already pairing just restarts discovery, no visual blank.
    // g_pending.linkState still holds the last PUBLISHED value here (this
    // iteration hasn't called publishState() yet), so this compares against
    // what core 1 actually last saw - same as reading g_state would.
    if (g_pending.linkState != LINK_PAIR) g_pending.pairBlankAtMs = millis();
    openPairingWindow();
  }
  if (buttonPressed(resetBtn)) {
    resetBondedController();
  }
}

void onConnectedController(ControllerPtr ctl) {
  bool bondedBefore = hasBondedController;
  int slot = -1;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] == nullptr) { controllers[i] = ctl; slot = i; break; }
  }
  if (slot < 0) {
    Serial.println("Controller connected but no empty slot");
    return;
  }

  // Guard: enableNewBluetoothConnections(false) does not always reject unbonded
  // controllers on the BT-Classic path. A connect with no bond and no open
  // window should be impossible - drop it.
  if (!bondedBefore && !pairingWindowOpen()) {
    Serial.println("Rejecting unexpected controller (no bond, not pairing)");
    controllers[slot] = nullptr;
    ctl->disconnect();
    return;
  }

  Serial.printf("Controller connected, slot %d\n", slot);
  if (pairingWindowOpen()) {
    for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
      if (i != slot && controllers[i]) {
        Serial.printf("Dropping stray controller in slot %d\n", i);
        controllers[i]->disconnect();
        controllers[i] = nullptr;
      }
    }
    persistBonded(true);   // authoritative-connect rule, ECU-SPEC-001 §8
    Serial.println("Bonded to this controller");
    closePairingWindow("new controller bonded");
  }
  digitalWrite(PIN_MODE_LED, HIGH);
}

void onDisconnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (controllers[i] == ctl) {
      Serial.printf("Controller disconnected, slot %d\n", i);
      controllers[i] = nullptr;
      digitalWrite(PIN_MODE_LED, LOW);
      break;
    }
  }
}

// Reduce the whole gamepad to the two axes the S4 stick-check HUD cares about:
// throttle = left stick vertical (up = forward, so negate axisY), steer = axisX.
void publishSticks() {
  int32_t thr = 0, str = 0;
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    ControllerPtr ctl = controllers[i];
    if (ctl && ctl->isConnected()) { thr = -ctl->axisY(); str = ctl->axisX(); break; }
  }
  g_pending.throttle = thr;
  g_pending.steer    = str;
}

LinkState computeLinkState() {
  if (anyControllerConnected())               return LINK_CONNECTED;
  if (pairingMode || !hasBondedController)     return LINK_PAIR;
  return LINK_SEARCH;
}

void btTask(void*) {
  Serial.printf("bt  started on core %d\n", xPortGetCoreID());
  logStack("bt ");

  BP32.setup(&onConnectedController, &onDisconnectedController);
  BP32.enableVirtualDevice(false);

  if (hasBondedController) {
    BP32.enableNewBluetoothConnections(false);   // Search: reconnect the bonded pad only
    Serial.println("bt  booted bonded -> Search");
  } else {
    Serial.println("bt  booted unpaired -> Pair");
    openPairingWindow();
  }
  Serial.printf("bt  Bluepad32 setup done on core %d (fw %s)\n",
                xPortGetCoreID(), BP32.firmwareVersion());
  logStack("bt ");

  uint32_t beats = 0, lastHeartbeat = 0;
  bool wasConnected = false;

  for (;;) {
    pollButtons();

    bool dataUpdated = BP32.update();
    (void)dataUpdated;
    publishSticks();

    // Deferred pairing-window setup (see openPairingWindow): run the BT-thread
    // key-wipe + discovery-enable only once the S2 feedback frame has had time
    // to reach the panel.
    if (g_pairSetupPendingAt &&
        millis() - g_pairSetupPendingAt >= PAIR_SETUP_DEFER_MS) {
      g_pairSetupPendingAt = 0;
      BP32.forgetBluetoothKeys();
      BP32.enableNewBluetoothConnections(true);
      Serial.println("Pairing window: keys forgotten, accepting new connections");
    }

    bool nowConnected = anyControllerConnected();
    if (nowConnected && !wasConnected) g_pending.connectedAtMs = millis();
    wasConnected = nowConnected;
    g_pending.connected  = nowConnected;
    g_pending.linkState  = computeLinkState();

    // One critical-section struct copy for this whole iteration's changes -
    // see the RobotState comment above.
    publishState();

    uint32_t now = millis();
    if (now - lastHeartbeat >= BT_HEARTBEAT_MS) {
      lastHeartbeat = now;
      Serial.printf("bt  alive on core %d  link=%s  (beat %lu)\n",
                    xPortGetCoreID(), LINK_NAME[g_pending.linkState], (unsigned long)beats);
      if ((beats % 10) == 9) logStack("bt ");
      beats++;
    }

    vTaskDelay(pdMS_TO_TICKS(BT_POLL_MS));
  }
}
