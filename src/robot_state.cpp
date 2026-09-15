#include "robot_state.h"

const char* LINK_NAME[] = {"SEARCH", "PAIR", "CONNECTED"};

const char* ringCmdFor(LinkState s) {
  switch (s) {
    case LINK_SEARCH:    return "SEARCH_BLINK";
    case LINK_PAIR:      return "PAIR_BLINK";
    case LINK_CONNECTED: return "CONNECTED";
  }
  return "OFF";
}

RobotState   g_state;
portMUX_TYPE g_stateMux = portMUX_INITIALIZER_UNLOCKED;
RobotState   g_pending;

void publishState() {
  portENTER_CRITICAL(&g_stateMux);
  g_state = g_pending;
  portEXIT_CRITICAL(&g_stateMux);
}

RobotState readState() {
  RobotState snap;
  portENTER_CRITICAL(&g_stateMux);
  snap = g_state;
  portEXIT_CRITICAL(&g_stateMux);
  return snap;
}
