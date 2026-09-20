// Pin assignments and tunable constants shared by every module.
#pragma once

#include <Arduino.h>

// ---- pins --------------------------------------------------------------------
// Buttons keep the BluetoothPairing test-rig wiring (active-low, internal
// pull-up). ECU-SPEC-001 §2 puts Reset on GPIO14 and Pair on an input-only pin
// on the final board; the perfboard rig this sprint runs on uses 15 / 13.
constexpr int PIN_MODE_LED   = 2;    // HIGH = connected, LOW = searching/pairing - binary only, no blink pattern coded
constexpr int PIN_PAIR_BTN   = 15;   // Pair button   (HIGH -> LOW edge = press)
constexpr int PIN_RESET_BTN  = 13;   // Reset button  (clears the stored bond)
constexpr int PIN_I2C_SDA    = 21;   // SH1106 OLED
constexpr int PIN_I2C_SCL    = 22;
constexpr int PIN_UART1_RX   = 16;   // UART1 <- TCU  (ECU-SPEC-001 §2)
constexpr int PIN_UART1_TX   = 17;   // UART1 -> TCU  (RING / BUZZ commands)

// Motor driver pins - 2x L298N (ECU-SPEC-001 §3). One motor per channel:
// front ENA/IN1/IN2 -> front-left, front ENB/IN3/IN4 -> front-right,
// rear ENA/IN1/IN2 -> rear-left, rear ENB/IN3/IN4 -> rear-right.
constexpr int PIN_FL_EN  = 25, PIN_FR_EN  = 26;
constexpr int PIN_FL_IN1 = 27, PIN_FL_IN2 = 14;
constexpr int PIN_FR_IN1 = 32, PIN_FR_IN2 = 33;
constexpr int PIN_RL_EN  = 18, PIN_RR_EN  = 19;
constexpr int PIN_RL_IN1 = 23, PIN_RL_IN2 = 13;
constexpr int PIN_RR_IN1 = 4,  PIN_RR_IN2 = 5;

// ---- tunables --------------------------------------------------------------
constexpr int32_t  AXIS_MAX        = 512;   // Bluepad32 stick full-scale
constexpr int32_t  S4_DEADZONE     = 60;    // ~12 % of AXIS_MAX (ECU-SPEC-002 §7.6 - tune on hw)
constexpr uint32_t S4_LINGER_MS    = 400;   // active axis re-centred -> hold before Idle
constexpr uint32_t BT_POLL_MS      = 5;     // BP32.update() cadence in btTask
constexpr uint32_t BT_HEARTBEAT_MS = 1000;
constexpr uint32_t UI_FRAME_MS     = 166;   // ~6 Hz (ECU-ADR-004)
constexpr uint32_t S3_HOLD_MS      = 1500;  // Connected screen dwell before S4
constexpr uint32_t RESET_TOAST_MS  = 1000;  // S5 dwell
constexpr uint32_t PAIR_BLANK_MS       = 600;  // header-only feedback frame after a Pair press
constexpr uint32_t PAIR_SETUP_DEFER_MS = 200;  // hold the BT key-wipe until that feedback frame is flushed
constexpr uint32_t BTN_DEBOUNCE_MS = 40;
constexpr int      BATT_PCT_STUB   = 82;    // no real SoC on the ECU (BMS owns it) - placeholder only

// Motor PWM (ECU-SPEC-001 §3/§4/§9 don't pin these down - firmware choice).
constexpr int      MOTOR_PWM_FREQ_HZ  = 20000;  // inaudible
constexpr int      MOTOR_PWM_RES_BITS = 8;      // 0-255 duty
constexpr uint32_t MOTOR_STALE_MS     = 250;    // zero motors if no new gamepad data this long
constexpr float    MOTOR_LIMP_RATIO   = 0.5f;   // used while tcuLinkUp == false

// ---- NVS (which controller this car is bonded to - ECU-SPEC-001 §8) --------
constexpr char NVS_NS[]         = "nitro-ecu";
constexpr char NVS_KEY_BONDED[] = "bonded";

// ---- misc shared helpers ----------------------------------------------------
void logStack(const char* who);
