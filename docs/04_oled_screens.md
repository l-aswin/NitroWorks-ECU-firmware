# OLED Screen Catalog

128x64 SH1106, driven by `uiTask` in [`oled_ui.cpp`](../src/oled_ui.cpp) at
`UI_FRAME_MS` (~6 Hz). Screens are a **verbatim port** of the pixel-accurate
`BluetoothUIMockup` reference (ECU-SPEC-002) — if geometry needs to change,
change the mockup first and copy it here; don't re-derive from the spec text.

Screen IDs (S0–S5) follow ECU-SPEC-002 §5 naming. Selection logic lives in
`uiTask()`'s render block; see the flowchart in
[03_control_flow.md](03_control_flow.md#uitask-loop-core-1-priority-1-every-ui_frame_ms--166-ms).

## Selection precedence (highest wins)

1. **S5 Reset toast** — if a Reset press happened within `RESET_TOAST_MS`
   (1000 ms), regardless of link state.
2. **S3 Connected** — `linkState == CONNECTED` and less than `S3_HOLD_MS`
   (1500 ms) since the connect edge.
3. **S4 Stick-check HUD** — `linkState == CONNECTED` after the S3 hold.
4. **S2 / S2b Pair** — `linkState == PAIR`.
5. **S1 Search/Scan** — `linkState == SEARCH` (the only remaining case).

S0 (Critical, full-screen takeover) is referenced in code comments as a
step-5 item — **not implemented yet** (`BattState` currently only ever gets
`BATT_NORMAL`, since S1–S4 always pass that in).

## Shared elements

- **Status strip** (`drawStatusStrip`, all screens except S4's HUD row and
  S5): battery glyph left (outline + proportional fill + `NN%`), optional
  Bluetooth glyph at the right edge when `hudBtIcon` is set, full-width
  divider line at y=20.
  - `BATT_NORMAL`: 32x14 outline, proportional fill bar, printed percentage
    (`BATT_PCT_STUB` = 82, since there's no real battery SoC on the ECU yet
    — BMS owns it).
  - `BATT_WARNING`: larger outline, blinking exclamation mark, constant
    "15%" text. *(Not currently reachable — no caller passes `BATT_WARNING`
    yet; wired for when a real low-battery signal exists.)*
- **Bluetooth glyph** (`drawBtGlyph`): 5-line glyph (stem + two shoulders +
  crossing diagonals). Drawn twice at a 1px offset when `bold` (used for the
  "connected" variant), with an optional filled dot to mark "actively
  connected" vs. "searching".
- **Searching waves** (`drawWaves`): up to 3 right-opening arcs at radii
  `step, 2*step, 3*step` from a center just right of the BT glyph; double-
  drawn (solid) for the connected variant.

## S1 — Search (`renderSearchLike("SCAN", ...)`)

Shown when `linkState == SEARCH` (bonded controller exists, not currently
connected — ECU is reconnecting to it in the background).

- Status strip, no BT icon in the strip.
- Word `"SCAN"`, size-3 text at (6, 31).
- Plain (non-bold, no dot) BT glyph at (97, 41).
- Animated waves (`animWaves()`, cycling 0–3 every 300 ms) — pulses outward
  to indicate active scanning.

## S2 / S2b — Pair (`renderSearchLike("PAIR", ...)` / header-only blank)

Shown when `linkState == PAIR` (pairing window open, or nothing bonded yet).

- **S2b** (blank feedback frame): for `PAIR_BLANK_MS` (600 ms) right after a
  Pair-button press, only the status strip is drawn — the rest of the
  screen is intentionally blank as immediate visual feedback that the press
  registered, before the "PAIR" word/glyph frame takes over.
- **S2**: same layout as S1 but with the word `"PAIR"` instead of `"SCAN"`.

## S3 — Connected (`renderConnected`)

Shown for `S3_HOLD_MS` (1500 ms) immediately after the connect edge
(`connectedAtMs`), before handing off to S4.

- `"READY"`, size-2 text at (34, 6) — no status strip drawn on this screen.
- Bold BT glyph with filled dot at (55, 40) — the "connected" glyph variant.
- Solid (double-drawn) waves, all 3 rings, at (66, 40).

## S4 — Stick-check HUD (`drawS4`)

Shown once `linkState == CONNECTED` and the S3 hold has elapsed. Live view
of gamepad throttle/steer, driven by `s4PickAxis()`.

**Axis arbitration** (`s4PickAxis`, in `oled_ui.cpp`):
- Both throttle and steer are compared against `S4_DEADZONE` (60, ≈12% of
  `AXIS_MAX`).
- If both cross the deadzone from Idle simultaneously, **throttle wins**
  (ECU-SPEC-002 §4).
- Once an axis is "held" (`ACC`/`REV`/`TURN_L`/`TURN_R`), it stays held
  until both axes drop back under the deadzone, then **lingers** for
  `S4_LINGER_MS` (400 ms) before returning to Idle — prevents flicker from
  a stick passing through center.

**States**:
- **Idle** (no axis held): status strip with BT icon, plus an idle gamepad
  glyph — rounded-rect body, d-pad cross, two face-button dots. No bar/label
  drawn.
- **ACC** (throttle > 0): label `"ACC"` + right-pointing filled triangle,
  right-aligned `NN%` magnitude, horizontal bar anchored left growing right.
- **REV** (throttle < 0): label `"REV"` + left-pointing triangle, bar
  anchored right growing left.
- **TURN L / TURN R** (steer dominant): label `"TURN L"`/`"TURN R"`, bar is
  center-out from a zero tick at the middle of the bar track — fills right
  for `TURN R`, left for `TURN L`.
- Magnitude `m` for whichever axis is active: `(abs(raw) - S4_DEADZONE) /
  (AXIS_MAX - S4_DEADZONE)`, clamped to [0,1] — i.e. rescaled so the
  deadzone edge is 0% and full deflection is 100%.
- Bar geometry: track at (4, 40), 120x16px, 2px inset for the fill.

## S5 — Reset toast (`drawResetToast`)

Shown for `RESET_TOAST_MS` (1000 ms) after a Reset-button press, overriding
every other screen. No status strip (ECU-SPEC-002 has no mockup entry for
this screen — geometry is original to this firmware, not ported).

- Text `"RESET"` plus 1–3 growing dots, cycling every 300 ms
  (`"RESET."` → `"RESET.."` → `"RESET..."`), size-2 at (18, 24).

## Screen ↔ RING/BUZZ correlation

Every `linkState` change (not every screen change — S3/S4/S5 are sub-states
of `CONNECTED` and don't re-trigger this) emits `RING <token>` via
`ringCmdFor()`:

| linkState | RING token |
|---|---|
| SEARCH | `SEARCH_BLINK` |
| PAIR | `PAIR_BLINK` |
| CONNECTED | `CONNECTED` |

`BUZZ CONNECT` is additionally emitted once, on the transition *into*
`CONNECTED`.
