# NitroWorks ECU Firmware — Overview

## Goal

The ECU firmware runs on an ESP32 and does three things:

1. Pairs and bonds a Bluetooth gamepad (Bluepad32), and re-pairs the same
   controller automatically after reboot.
2. Reads the gamepad's throttle/steer input and drives a 4-motor skid-steer
   chassis via PWM (2x L298N motor drivers).
3. Shows link status (searching / pairing / connected) and a live stick-input
   HUD on a 1.3" SH1106 OLED, and emits RING/BUZZ commands to a downstream
   TCU over UART1.

References: `ECU-SPEC-001` (electrical/protocol spec), `ECU-SPEC-002` (OLED UI
spec) — cited in code comments throughout.

## Hardware map

See [`config.h`](../include/config.h) for the authoritative pin list. Summary:

| Function | Pins |
|---|---|
| Mode LED | GPIO2 |
| Pair / Reset buttons | GPIO15 / GPIO13 (active-low, internal pull-up) |
| OLED (I2C) | SDA 21, SCL 22 |
| UART1 to TCU | RX 16, TX 17 |
| Motors (2x L298N) | FL: EN25/IN1 27/IN2 14, FR: EN26/IN1 32/IN2 33, RL: EN18/IN1 23/IN2 13, RR: EN19/IN1 4/IN2 5 |

Motor PWM: 20 kHz, 8-bit duty (`MOTOR_PWM_FREQ_HZ`, `MOTOR_PWM_RES_BITS`).

## Architecture: two cores, three tasks

The ESP32 has two cores; each gets a pinned FreeRTOS task, plus a third task
sharing core 1:

```
core 0                          core 1
┌──────────────┐                ┌──────────────┐   ┌────────────────────┐
│   btTask      │  notify        │   uiTask      │   │  motorControlTask  │
│  (priority 3) │ ─────────────► │ (priority 1)  │   │   (priority 5)     │
│               │                │               │   │                    │
│ Bluepad32     │                │ OLED render   │   │ Skid-steer mix     │
│ pairing/bond  │                │ RING/BUZZ TX  │   │ PWM output         │
│ gamepad poll  │                └──────────────┘   └────────────────────┘
└──────────────┘
       │                                                      ▲
       └──────────────────── g_state (RobotState) ────────────┘
              published under g_stateMux, read as a snapshot
```

`setup()` in [`main.cpp`](../src/main.cpp) spawns all three tasks after
seeding shared state from NVS.

## Cross-core state sharing

All communication between cores goes through one struct: `RobotState`
(defined in [`robot_state.h`](../include/robot_state.h)). It carries link
state, bond/connection flags, throttle/steer, and timing marks for UI
transitions.

- **`g_pending`** — core-0-only working copy. `btTask` reads/writes it freely
  with no locking (nothing else touches it), accumulating this iteration's
  changes.
- **`g_state`** — the shared copy. Written only via `publishState()`, read
  only via `readState()`, both of which take `g_stateMux` (a spinlock) for
  the duration of a single struct copy — not per-field. This keeps the
  critical section short and guarantees every reader sees a consistent,
  single-iteration snapshot rather than a mix of old/new fields.

`btTask` calls `publishState()` once per loop iteration, then wakes
`motorControlTask` via `xTaskNotifyGive`. `uiTask` and `motorControlTask`
each take their own `readState()` snapshot once per frame/cycle and render
or actuate off that snapshot alone.

## Persistence

The bonded-controller flag is the only thing persisted (NVS, namespace
`nitro-ecu`, key `bonded`). Everything else is runtime-only and resets on
reboot except the bond, so the firmware reconnects to the same gamepad
automatically.

See [`02_functionality.md`](02_functionality.md) for a per-module reference
and [`03_control_flow.md`](03_control_flow.md) for flowcharts of each loop
and the link-state machine.
