// core 1: OLED rendering (ECU-SPEC-002) + RING/BUZZ command emit.
#pragma once

// FreeRTOS task entry point, pinned to core 1 (see setup()).
void uiTask(void*);
