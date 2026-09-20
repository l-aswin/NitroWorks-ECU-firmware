// 4-motor PWM drive (2x L298N) - ECU-SPEC-001 §4/§6. Core 1, woken by btTask
// via xTaskNotifyGive on new gamepad data, with a 10 ms timeout fallback.
#pragma once

#include <Arduino.h>

extern TaskHandle_t g_motorTaskHandle;

void motorControlTask(void*);
