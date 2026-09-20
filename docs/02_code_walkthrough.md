# Module Reference

## `main.cpp`

Wires everything together; contains no logic of its own beyond boot.

- `setup()`: starts `Serial`, initializes both buttons, opens UART1, loads
  `hasBondedController` from NVS, seeds `g_pending`/`g_state` via
  `publishState()`, then spawns:
  - `btTask` → core 0, priority 3, 8192-word stack
  - `uiTask` → core 1, priority 1, 4096-word stack
  - `motorControlTask` → core 1, priority 5, 4096-word stack (handle stored
    in `g_motorTaskHandle` so `btTask` can notify it)
- `loop()`: empty — all work happens in the three pinned tasks.

## `config.h`

No logic — pin assignments and tunable constants shared by every module
(motor pins, PWM frequency/resolution, deadzone, timing constants for UI
transitions, NVS keys). Single source of truth; nothing here should be
hardcoded elsewhere.

## `robot_state.h` / `robot_state.cpp`

Defines the cross-core `RobotState` struct and the publish/read mechanism
described in [01_overview.md](01_overview.md#cross-core-state-sharing).

- `LinkState` enum: `LINK_SEARCH`, `LINK_PAIR`, `LINK_CONNECTED`.
- `ringCmdFor(LinkState)`: maps link state to the RING command token sent to
  the TCU (`SEARCH_BLINK` / `PAIR_BLINK` / `CONNECTED`).
- `publishState()` / `readState()`: the only sanctioned way to move data
  between `g_pending` and `g_state`; both take `g_stateMux`.

## `bt_task.cpp`

Owns Bluepad32 pairing/bonding and gamepad polling. Runs entirely on core 0.

- **Pairing window** (`openPairingWindow` / `closePairingWindow`): opening
  disconnects any active controller, flips `linkState` to `PAIR`, and defers
  the actual NVS key-wipe + `enableNewBluetoothConnections(true)` by
  `PAIR_SETUP_DEFER_MS` so the OLED's press-feedback frame isn't stalled by
  the flash-erase freeze that key-wipe causes.
- **Bonding** (`onConnectedController`): rejects unexpected connections
  (no bond + no open pairing window), persists the bond to NVS on a
  successful pair inside an open window (`persistBonded(true)`), and drops
  stray extra controllers.
- **Buttons** (`pollButtons`): Pair button re-opens the pairing window;
  Reset button clears the stored bond (`resetBondedController`) and also
  opens pairing, plus triggers the S5 reset toast on the OLED.
- **Gamepad → axes** (`publishSticks`): reads the *first* connected
  controller's left-stick Y (throttle, negated so up = forward) and
  right-stick X (steer), writing them to `g_pending.throttle` /
  `g_pending.steer`. See [`03_control_flow.md`](03_control_flow.md) for the
  full gamepad → PWM data flow.
- **Link state** (`computeLinkState`): `CONNECTED` if any controller is
  connected, else `PAIR` if a pairing window is open or nothing is bonded
  yet, else `SEARCH` (waiting to reconnect a known bond).
- **`btTask` loop**: poll buttons → `BP32.update()` → `publishSticks()` →
  run any deferred pairing setup → recompute link state →
  `publishState()` → notify `motorControlTask`. Runs every `BT_POLL_MS`
  (5 ms).

## `motor_control.h` / `motor_control.cpp`

Owns the 4 motor PWM channels. Runs on core 1, woken by `btTask`'s
notification with a 10 ms timeout fallback.

- Sets up 4 `ledc` PWM channels (one per wheel) at `MOTOR_PWM_FREQ_HZ` /
  `MOTOR_PWM_RES_BITS`, plus direction pins per motor.
- `setMotor()`: sets IN1/IN2 direction pins from the sign of a signed duty
  value, writes `abs(duty)` to the PWM channel.
- **Stale-input safety**: if `millis() - snap.inputAtMs > MOTOR_STALE_MS`
  (250 ms since the last new gamepad frame), calls `allStop()` instead of
  driving — protects against a dropped Bluetooth link leaving motors
  running.
- **Skid-steer mix**: `left = throttle + steer`, `right = throttle - steer`,
  clamped to `±AXIS_MAX`, then scaled by a degradation ratio (from the TCU
  link, currently stubbed at 1.0 / `MOTOR_LIMP_RATIO` until UART parsing
  lands) and converted to PWM duty. Front/rear motors on the same side share
  one duty value.

## `oled_ui.h` / `oled_ui.cpp`

Owns the SH1106 OLED render loop and RING/BUZZ emission. Runs on core 1.
Draw helpers are a verbatim port of a pixel-accurate mockup — **don't
re-derive geometry from the spec text; edit the mockup first, then copy
here.**

- Screen selection per frame, driven by the `RobotState` snapshot:
  - Reset toast (`drawResetToast`) if a Reset press happened within
    `RESET_TOAST_MS`.
  - `LINK_CONNECTED`: `renderConnected()` (S3, "READY") for `S3_HOLD_MS`
    after connecting, then the S4 stick-check HUD (`drawS4`).
  - `LINK_PAIR`: header-only blank frame right after a Pair press
    (`PAIR_BLANK_MS`), else `renderSearchLike("PAIR", ...)`.
  - else: `renderSearchLike("SCAN", ...)` (searching for the bonded pad).
- `s4PickAxis()`: turns live throttle/steer into one of
  `IDLE/ACC/REV/TURN_L/TURN_R` for the S4 HUD, with a deadzone
  (`S4_DEADZONE`) and a ~400 ms linger back to Idle so the display doesn't
  flicker between axes.
- **RING/BUZZ emit**: on every `linkState` change, sends `RING <token>` via
  `emitCmd`; additionally sends `BUZZ CONNECT` on transition to
  `LINK_CONNECTED`. Also pumps any UART1 RX each frame (`pumpUart1Rx`).
- Runs at `UI_FRAME_MS` (~6 Hz) cadence.

## `uart_link.h` / `uart_link.cpp`

Thin wrapper around `Serial1` to the TCU (currently a loopback/serial-print
stub, no real TCU parsing yet).

- `emitCmd(cmd)`: writes a line to `Serial1` and echoes it to `Serial` as
  `tx> ...`.
- `pumpUart1Rx()`: line-buffers incoming bytes from `Serial1`, printing
  completed lines to `Serial` as `rx< ...`. No command parsing yet — this is
  where TCU degradation-ratio/heartbeat parsing (referenced as a TODO in
  `robot_state.h`) will eventually live.

## `button.h` / `button.cpp`

Generic debounced active-low button helper, used for Pair/Reset.

- `initButton()`: configures `INPUT_PULLUP`, seeds stable/last-reading state.
- `buttonPressed()`: returns `true` exactly once per debounced HIGH→LOW
  edge (`BTN_DEBOUNCE_MS` settle time).

## `log_util.cpp`

One helper, `logStack(who)`, printing the calling task's FreeRTOS stack
high-water mark in bytes. Called periodically by all three tasks to catch
stack-size regressions during development.
