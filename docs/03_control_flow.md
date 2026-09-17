# Control Flow

## Boot sequence

```mermaid
flowchart TD
    A[setup] --> B[Init Serial, buttons, UART1]
    B --> C[prefs.begin NVS]
    C --> D[hasBondedController = NVS bonded flag]
    D --> E[g_pending.linkState = bonded ? SEARCH : PAIR]
    E --> F[publishState - seed g_state]
    F --> G[spawn btTask -> core 0]
    F --> H[spawn uiTask -> core 1]
    F --> I[spawn motorControlTask -> core 1]
    G --> J[loop - idle, vTaskDelay 1s]
    H --> J
    I --> J
```

## `btTask` loop (core 0, every `BT_POLL_MS` = 5 ms)

```mermaid
flowchart TD
    Start([loop iteration]) --> PB[pollButtons]
    PB -->|Pair pressed| OPW[openPairingWindow]
    PB -->|Reset pressed| RBC[resetBondedController -> persistBonded false, openPairingWindow]
    PB --> BPU[BP32.update]
    BPU --> PS[publishSticks: read left stick Y -> throttle, right stick X -> steer]
    PS --> DU{dataUpdated?}
    DU -->|yes| IAM[g_pending.inputAtMs = millis]
    DU -->|no| DEF
    IAM --> DEF{pairing setup deferred and due?}
    DEF -->|yes| KW[forgetBluetoothKeys + enableNewBluetoothConnections true]
    DEF -->|no| CLS
    KW --> CLS[computeLinkState]
    CLS --> PUB[publishState: g_state = g_pending under g_stateMux]
    PUB --> NOTIFY[xTaskNotifyGive motorControlTask]
    NOTIFY --> HB{heartbeat interval elapsed?}
    HB -->|yes| LOG[Serial heartbeat + occasional logStack]
    HB -->|no| DELAY
    LOG --> DELAY[vTaskDelay BT_POLL_MS]
    DELAY --> Start
```

## `LinkState` state machine

```mermaid
stateDiagram-v2
    [*] --> SEARCH: boot, hasBondedController true
    [*] --> PAIR: boot, hasBondedController false
    SEARCH --> PAIR: Pair button / Reset button pressed
    PAIR --> CONNECTED: controller bonds inside open pairing window
    SEARCH --> CONNECTED: bonded controller reconnects
    CONNECTED --> SEARCH: controller disconnects
    CONNECTED --> PAIR: Pair button / Reset button pressed
```

`computeLinkState()` in `bt_task.cpp` re-evaluates this every iteration from
`anyControllerConnected()`, `pairingMode`, and `hasBondedController` — it's
not an explicit transition table, but the effective behavior matches the
diagram above.

## `motorControlTask` loop (core 1, priority 5)

```mermaid
flowchart TD
    Start([loop iteration]) --> Wait[ulTaskNotifyTake - wait for btTask notify, 10ms timeout]
    Wait --> Snap[snap = readState under g_stateMux]
    Snap --> Stale{millis - snap.inputAtMs > MOTOR_STALE_MS?}
    Stale -->|yes| AllStop[allStop - zero all 4 channels]
    Stale -->|no| Mix[left = throttle+steer, right = throttle-steer, clamp to AXIS_MAX]
    Mix --> Ratio[ratio = tcuLinkUp ? degradationRatio : MOTOR_LIMP_RATIO]
    Ratio --> Duty[leftDuty/rightDuty = axis * ratio * DUTY_MAX / AXIS_MAX]
    Duty --> Drive[setMotor x4: FL/RL = leftDuty, FR/RR = rightDuty]
    AllStop --> Log
    Drive --> Log{cycle % 300 == 299?}
    Log -->|yes| LogStack[logStack]
    Log -->|no| Start
    LogStack --> Start
```

## `uiTask` loop (core 1, priority 1, every `UI_FRAME_MS` ≈ 166 ms)

```mermaid
flowchart TD
    Start([loop iteration]) --> Snap[snap = readState under g_stateMux]
    Snap --> Flags[compute showReset / pairBlank from timing fields]
    Flags --> Emit{linkState changed since last frame?}
    Emit -->|yes| Ring[emitCmd RING token; BUZZ CONNECT if now CONNECTED]
    Emit -->|no| Pump
    Ring --> Pump[pumpUart1Rx]
    Pump --> Render{pick screen}
    Render -->|showReset| S5[drawResetToast]
    Render -->|CONNECTED, within S3_HOLD_MS| S3[renderConnected]
    Render -->|CONNECTED, after hold| S4[drawS4 throttle, steer]
    Render -->|PAIR, pairBlank| S2b[header-only strip]
    Render -->|PAIR| S2[renderSearchLike PAIR]
    Render -->|SEARCH| S1[renderSearchLike SCAN]
    S5 --> Disp[display.display]
    S3 --> Disp
    S4 --> Disp
    S2b --> Disp
    S2 --> Disp
    S1 --> Disp
    Disp --> Delay[vTaskDelay UI_FRAME_MS]
    Delay --> Start
```

## End-to-end: gamepad stick → motor PWM

```mermaid
flowchart LR
    LStick[Left stick Y axis] -->|negated| Thr[g_pending.throttle]
    RStick[Right stick X axis] --> Str[g_pending.steer]
    Thr --> Pub[publishState under g_stateMux]
    Str --> Pub
    Pub --> State[g_state]
    State -->|readState| MC[motorControlTask]
    MC --> MixCalc[left = thr+str, right = thr-str, clamp]
    MixCalc --> Scale[scale by degradation/limp ratio]
    Scale --> PWM[ledcWrite x4 - 20kHz, 8-bit duty]
    PWM --> Motors[4x DC motors via 2x L298N]
```

This is the path documented in `bt_task.cpp:publishSticks()` and
`motor_control.cpp:motorControlTask()` — see
[02_functionality.md](02_functionality.md) for the per-function detail
behind each box.
