#include "uart_link.h"
#include <Arduino.h>

void emitCmd(const char* cmd) {
  Serial1.println(cmd);
  Serial.printf("tx> %s\n", cmd);
}

void pumpUart1Rx() {
  static char line[64];
  static size_t n = 0;
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n' || c == '\r') {
      if (n) { line[n] = 0; Serial.printf("rx< %s\n", line); n = 0; }
    } else if (n < sizeof(line) - 1) {
      line[n++] = c;
    }
  }
}
