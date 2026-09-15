// Stack high-water logging helper shared by both core tasks.
#include "config.h"

void logStack(const char* who) {
  Serial.printf("%s  stack high-water: %u bytes free\n",
                who, (unsigned)uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t));
}
