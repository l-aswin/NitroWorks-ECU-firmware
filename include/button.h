// Generic debounced active-low button, used for the Pair/Reset buttons.
#pragma once

#include <Arduino.h>

struct Button {
  int pin;
  int stable;
  int lastReading;
  uint32_t lastChangeMs;
};

void initButton(Button& b);

// Returns true exactly once per press (debounced HIGH -> LOW edge).
bool buttonPressed(Button& b);
