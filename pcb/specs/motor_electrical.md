# Motor electrical data (P2-1, blueprint diff B-P2-02)

Source of the readings: `pcb/mechanical/measurements.md`, group G. This file holds the readings and the limits derived from them for P2-2 to P2-4. Every datasheet figure is labelled *verify against datasheet* until it is checked in P2-3 against a file listed in `pcb/datasheets/MANIFEST.md`.

## 1. Method and deviations

| Item | Value | Tag |
|---|---|---|
| Date | 2026-11-12 | [measured] |
| Meter | DT9205A, 20 A range (10 mA resolution); the meter has no 10 A range | [measured] |
| Circuit | Flight pack (DWZ 802540, 3.88 V open circuit) straight across one motor, meter in series, OEM board disconnected | [measured: DT9205A, 2026-11-12] |
| Stall | Shaft held, reading taken within 1 to 2 s | [measured] |
| Running | Same circuit, motor spinning freely with its prop on, full pack voltage, no PWM | [measured] |

**Deviations from package §4 step 6 (accepted at the P2-1 gate, 2026-11-20):**
1. **One stall reading per motor, not three.** The repeats are owed and are taken in Phase 3 step 2 on the final board, through the fuse.
2. **No inline fuse.** None was available.
3. **Flight pack instead of a protected 18650.** The pack voltage under load was not read, so the readings include the pack's sag.
4. **20 A range instead of 10 A.** The meter has no 10 A range.
5. **Props on for the running readings.**

## 2. Readings

| Motor | Position | Stall (A) | Running, prop on (A) | Tag |
|---|---|---|---|---|
| M1 | front-right | 2.92 | 1.50 | [measured: DT9205A 20 A, 2026-11-12] |
| M2 | rear-right | 2.93 | 1.45 | [measured: DT9205A 20 A, 2026-11-12] |
| M3 | rear-left | 2.93 | 1.50 | [measured: DT9205A 20 A, 2026-11-12] |
| M4 | front-left | 2.95 | 1.57 | [measured: DT9205A 20 A, 2026-11-12] |
| **Highest** | | **2.95** (M4) | **1.57** (M4) | |
| Sum of four | | 11.73 | 6.02 | [derived] |

These positions are the OEM labels used in group G. Blueprint §3.4 numbers the motors differently for the firmware (Motor 1 is front left). The P2-4 pin map reconciles the two.

## 3. Derived design currents

| Quantity | Value | Formula or basis | Tag |
|---|---|---|---|
| Circuit resistance at stall | 1.32 Ω | 3.88 V / 2.95 A. Includes the winding, the leads, the meter shunt and the pack's internal resistance | [derived] |
| Stall at a stiff 4.20 V supply | 3.19 A | 4.20 V / 1.32 Ω. This is low, because the pack's resistance is no longer in the loop | [derived] |
| **Per-motor stall design value** | **3.5 A** | 3.19 A plus about 10 % for the pack, meter and lead resistance that the final board will not have | [assumed margin] |
| Per-motor continuous (full throttle) | 1.57 A | Highest running reading with the prop on (100 % duty, no PWM) | [measured] |
| **Shared battery path, full throttle** | **6.0 A** | Sum of the four running readings, before the logic load (about 0.3 to 0.5 A with Wi-Fi active) | [derived] |
| Shared battery path, all four stalled | 14 A | 4 × 3.5 A. A fault case only: a crash with all props blocked, cleared by the fuse and the firmware cut | [derived] |

## 4. Trace width (IPC-2221, external layer)

$I = 0.048\,\Delta T^{0.44}A^{0.725}$, with I in A, ΔT in °C and A the cross-section in mil² (*verify against IPC-2221*). The calculation uses ΔT = 10 °C and copper weights of 1 oz = 1.378 mil (35 µm) and 2 oz = 2.756 mil (*verify against the fab's stated thickness*).

| Current | A (mil²) | Width, 1 oz | Width, 2 oz | Use |
|---|---|---|---|---|
| 1.57 A (running, one motor) | 30.4 | 0.56 mm | 0.28 mm | Not used for sizing: stall governs |
| **2.95 A (highest stall measured)** | 72.5 | **1.34 mm** | 0.67 mm | Package criterion (highest peak at a 10 °C rise) |
| **3.5 A (per-motor stall design)** | 91.7 | **1.69 mm** | 0.85 mm | Recommended per-motor trace: motor pad, MOSFET drain, flyback diode |
| 6.0 A (shared path, full throttle) | 193.8 | 3.57 mm | 1.79 mm | Battery connector to the star point, and the motor positive rail |
| 14 A (shared path, all four stalled) | 620.8 | 11.4 mm | 5.7 mm | Does not fit a 23.5 mm board as a trace. Use pours on both layers. A short stall is a transient, not a steady 10 °C case. Decide in P2-2 |

The IPC-2221 curve describes a steady-state temperature rise. A stall lasts only until the firmware cut or the fuse acts, so sizing the per-motor traces for 3.5 A at 10 °C is conservative. The routing rule (P2-4) therefore sets per-motor traces at least 1.7 mm on 1 oz copper, or pours. The shared path is a pour on both layers with stitching vias.

## 5. Part margins

| Part | Rating (*verify against datasheet*) | Load | Margin | Note |
|---|---|---|---|---|
| AO3400A, continuous Id | about 5.7 A at TA = 25 °C (blueprint §3.2) | 3.5 A stall, 1.57 A running | 1.6× at stall, 3.6× running | The rating is at an ambient temperature on a specified copper area. Re-check the derating at 70 °C in P2-3 |
| AO3400A, dissipation at Rds(on) 48 mΩ (VGS 2.5 V) | Pd limit from the datasheet, on its test copper | 0.59 W at 3.5 A, 0.12 W at 1.57 A | Check in P2-3 | At a 3.3 V gate drive, Rds(on) lies between the 2.5 V and 4.5 V figures (33 mΩ gives 0.40 W at 3.5 A). Stall dissipation needs copper area at each drain |
| SS34, average forward current | 3 A (blueprint §3.2) | Freewheel ≈ I_motor × (1 − D): at most about 1.6 A running; up to 3.5 A momentarily at stall with low duty | Running 1.9×; stall at or above the rating | Acceptable as a transient. Confirm the surge rating (IFSM) and thermal data in P2-3 |

## 6. Fuse suggestion

| Fuse | Rating | Basis | Tag |
|---|---|---|---|
| Bench, single motor (Phase 3) | 5 A | Above the 3.5 A single-motor stall, within the blueprint's 5 to 7.5 A bench rule (§3.1) | [derived] |
| Board main fuse (flight) | **about 10 A** | Above the 6.0 A full-throttle total plus about 0.5 A logic and inrush, and below the 14 A four-motor stall. Part, I²t and hold or trip curve chosen in P2-3 with the datasheet. Mass and height checked in P2-2 | [assumed: choice to be confirmed in P2-3] |

## 7. Open

- Three repeat stall readings per motor, with the fuse fitted: Phase 3 step 2.
- Pack voltage under stall, and the pack's internal resistance: read during the Phase 3 repeats.
- The fault-case current of 14 A on the shared path: pour strategy and fuse choice in P2-2.
- Every datasheet figure above: P2-3.
