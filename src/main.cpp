#include <Arduino.h>
#include <string.h>
#include <Preferences.h>
#include <Bluepad32.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>


static void logStack(const char* who) {
  Serial.printf("%s  stack high-water: %u bytes free\n",
                who, (unsigned)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
}

// ---- core 0: Bluetooth / pairing task -----------------------------------
void btTask(void*) {
  Serial.printf("bt started on core %d\n", xPortGetCoreID());
  logStack("bt ");
}

// ---- core 1 ----- //
void uiTask(void*) {
  Serial.printf("ui started on core %d\n", xPortGetCoreID());
  logStack("ui ");
}

void setup() {
  Serial.begin(115200);
  Serial.printf("free heap before tasks: %u B  (largest block %u B)\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  xTaskCreatePinnedToCore(btTask, "bt", 8192, nullptr, 3, nullptr, 0);  // core 0
  xTaskCreatePinnedToCore(uiTask, "ui", 4096, nullptr, 1, nullptr, 1);  // core 1
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));   // all work is in the two pinned tasks
}

