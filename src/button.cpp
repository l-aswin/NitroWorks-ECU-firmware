#include "button.h"
#include "config.h"

void initButton(Button& b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.stable = b.lastReading = digitalRead(b.pin);
  b.lastChangeMs = millis();
}

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
