// NitroWorks ECU - DualCore sprint - STEP 4: mutex-protected RobotState + snapshot
//                                            (pairing state machine core 0 / OLED core 1 unchanged)
//
// This file only wires the modules together: load NVS, seed the shared
// RobotState, then spawn btTask (core 0) and uiTask (core 1). See:
//   config.h       - pins & tunables
//   robot_state.h  - cross-core RobotState struct + publish/read
//   button.h       - debounced button helper
//   bt_task.h      - Bluepad32 pairing/bonding state machine (core 0)
//   oled_ui.h      - OLED rendering + RING/BUZZ emit (core 1)
//   motor_control.h - 4-motor PWM skid-steer drive (core 1)
//   uart_link.h    - UART1 command emit/receive

#include <Arduino.h>

#include "bt_task.h"
#include "config.h"
#include "button.h"
#include "motor_control.h"
#include "oled_ui.h"
#include "robot_state.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(PIN_MODE_LED, OUTPUT);
  digitalWrite(PIN_MODE_LED, LOW);
  initButton(pairBtn);
  initButton(resetBtn);

  // UART1 to the TCU - RX 16 / TX 17 (ECU-SPEC-001 §2). Jumper 17->16 for loopback.
  Serial1.begin(115200, SERIAL_8N1, PIN_UART1_RX, PIN_UART1_TX);

  prefs.begin(NVS_NS, false);
#ifdef NITRO_QA_RESET_NVS
  prefs.clear();
  Serial.println("NITRO_QA_RESET_NVS: cleared stored bond");
#endif
  hasBondedController = prefs.getBool(NVS_KEY_BONDED, false);
  g_pending.hasBond   = hasBondedController;
  g_pending.linkState = hasBondedController ? LINK_SEARCH : LINK_PAIR;
  publishState();   // both tasks are created below, but seed g_state before they run either way

  Serial.println();
  Serial.println("NitroWorks ECU - DualCore sprint - STEP 4 (mutex-protected RobotState + snapshot)");
  Serial.printf("setup() runs on core %d\n", xPortGetCoreID());
  Serial.printf("Boot: NVS bonded flag = %d\n", hasBondedController);
  Serial.printf("free heap before tasks: %u B  (largest block %u B)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  xTaskCreatePinnedToCore(btTask, "bt", 8192, nullptr, 3, nullptr, 0);  // core 0
  xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 1, nullptr, 1);  // core 1
  xTaskCreatePinnedToCore(motorControlTask, "motor", 4096, nullptr, 5, &g_motorTaskHandle, 1);  // core 1
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));   // all work is in the two pinned tasks
}
