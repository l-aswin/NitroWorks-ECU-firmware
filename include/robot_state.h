// Cross-core shared state: core 0 (btTask) writes, core 1 (uiTask) reads.
// All access to g_state goes through publishState()/readState(), which
// hold portMUX_TYPE g_stateMux only for a single struct copy - see the
// comments on g_pending/g_state below for why.
#pragma once

#include <Arduino.h>

// ---- link state ----------------------------------------------------------
enum LinkState : uint8_t { LINK_SEARCH = 0, LINK_PAIR = 1, LINK_CONNECTED = 2 };
extern const char* LINK_NAME[];

// RING command token per link state (ECU-SPEC-001 §4 grammar).
const char* ringCmdFor(LinkState s);

struct RobotState {
  LinkState linkState     = LINK_PAIR;
  bool      hasBond       = false;
  bool      connected     = false;
  int32_t   throttle      = 0;   // +forward / -reverse, ~AXIS_MAX scale
  int32_t   steer         = 0;   // +right / -left
  uint32_t  connectedAtMs  = 0;  // millis() of the last 0->1 connect edge
  uint32_t  resetToastAtMs = 0;  // millis() of the last Reset press (0 = none)
  uint32_t  pairBlankAtMs  = 0;  // millis() of the last Pair press (0 = none) - drives the S2 feedback frame
};

extern RobotState   g_state;      // the shared copy - touch only under g_stateMux
extern portMUX_TYPE g_stateMux;   // spinlock guarding g_state

// core-0-only working copy - btTask reads/writes this freely with no locking
// (nothing else ever touches it), then flushes the whole thing into g_state
// under the spinlock once per loop iteration via publishState(). This keeps
// the critical section to a single struct copy instead of one per field.
extern RobotState g_pending;

void publishState();

// core-1-only: one locked copy of g_state per uiTask frame. uiTask renders the
// whole frame off the returned snapshot, so every field it reads comes from
// the same btTask iteration.
RobotState readState();
