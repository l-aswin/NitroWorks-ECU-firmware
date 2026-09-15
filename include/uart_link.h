// UART1 command emit/receive (loopback / serial-print stub) to the TCU.
#pragma once

void emitCmd(const char* cmd);
void pumpUart1Rx();
