// core 0: Bluepad32 gamepad pairing/connection state machine.
#pragma once

#include <Preferences.h>

#include "button.h"

extern Preferences prefs;
extern bool hasBondedController;   // mirror of the NVS flag, loaded in setup()
extern Button pairBtn;
extern Button resetBtn;

void persistBonded(bool v);

// FreeRTOS task entry point, pinned to core 0 (see setup()).
void btTask(void*);
