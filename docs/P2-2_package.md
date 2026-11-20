# Franken-E58 Phase 2, Stage P2-2: Architecture decisions

**Scope:** close every open architecture decision before any part is chosen or any schematic is drawn: module variant, assembly method, USB power path, IMU isolation, USB-C edge treatment, ToF connection, board stack-up and copper, shared battery path and fuse. Produce the mass budget, the IMU placement rules and `pcb/specs/requirements.md`. Fold the approved decisions into the blueprint. No schematic, no net table, no KiCad design edits in P2-2.

Blueprint sections used: §2.1, §3.1 to §3.5, §4.8, Phase 2 (decision table D-P2-01 to D-P2-08, steps 1 to 4), O2 to O8, R2, R10, R11, Phase 3 step 2. Also read `docs/P2-1_package.md` (§7, §8), `pcb/specs/motor_electrical.md`, `pcb/mechanical/measurements.md`, `pcb/mechanical/constraints.csv` and `pcb/decision_log.md`.

**Status: P2-2 in progress.** Prompt 1 (carry-in C1 to C6) done 2026-11-20; `outline.dxf` hole update (1.5 mm) pending from the user. Next: Prompt 2, decision session.

**Credentials:** none.

**Rules for this stage**
- The PCB rules in `CLAUDE.md` apply: every number tagged `[measured]`, `[datasheet p.N]` or `[assumed]`; no guessing; any figure recalled from memory is labelled *verify against datasheet*.
- **One decision at a time.** For each decision Claude Code states the options, shows the analysis, gives a recommendation with its reason, asks the user, and only then records the answer in `decision_log.md`. It never records a recommendation as a decision.
- A decision the user cannot make yet is **deferred with a named trigger** (what must be measured or read first), not guessed.
- Blueprint edits are shown as precise diffs (ID, old, new, reason) and applied only after approval.
- Battery and power reminders still apply to any bench step: protected high-drain cell, inline fuse, props off, attended.

---

## 0. Carry-in items from P2-1 (resolve first, in this order)

| # | Item | Why it blocks P2-2 | Action |
|---|---|---|---|
| C1 | Front wall: about 10 mm ahead of the board by ruler, 30 mm in the model | Decides the antenna edge, the USB-C edge and the usable board length | The user re-measures with the ruler or calipers. Claude Code corrects `constraints.csv` and the keep-outs |
| C2 | Wall top height (3.5 mm against 4.5 mm) and the motor-lead slots, missing or unconfirmed in the model | Component height limits and lead routing | The user checks the frame and confirms |
| C3 | Mounting holes leave a **0.2 mm rim** to the board edge | Almost certainly below the fab's minimum hole-to-edge distance and too fragile | Read the fab's capability page (hole to edge, copper to edge, minimum board size, 0.8 mm thickness option), record it in `pcb/specs/design_rules.md` (draft), and propose an outline or hole change if needed. Mark every fab figure with its source and date |
| C4 | Whether the bare board binds on the pegs | The soft-mount option changes the hole and ring geometry | The user tests with the 1:1 printout or the OEM board on the pegs and reports |
| C5 | All masses assumed (85 g total, 98 g with 15 % margin) | The mass budget and the flight-pack choice rest on them | The user weighs the OEM board, frame, battery, motors with props, camera and wiring. A scale that reads to 0.1 g is the minimum for the 6 g board, and 0.01 g is better |
| C6 | Fuse: the P2-1 suggestion of about 10 A against the blueprint bench rule of 5 to 7.5 A for first tests | The two figures answer different questions (aircraft protection against first-bring-up protection) and must not be mixed | Settle in decision D-P2-13 below and record both figures with their purpose |

## 1. Decisions to close

Recommended defaults are Claude Code's starting proposals, to be confirmed by the user.

| ID | Decision | Options | Recommended default and reason |
|---|---|---|---|
| **D-P2-02 / O3** | Module variant | N16R8, N8, MINI | **N16R8**: it is the validated family (flash and PSRAM modes, `sdkconfig`). Same footprint class as N8, so no space is saved. Any other variant means re-verifying the §4.9 options. Compare mass, size, reserved GPIOs and stock from the datasheet and the JLCPCB database |
| **D-P2-07 / O6** | Assembly method | Hand-solder everything; fab assembly of fine-pitch parts and hand-fit of the rest; full fab assembly | **Fab assembly of the module, IMU and USB-C receptacle, hand-fit of connectors, wires and large parts**, because the module's ground pad and the IMU QFN need hot air or reflow. If the user owns hot air and paste, a hand-solder plan is acceptable: record it. Claude Code lists each IC's package and hand-solder feasibility |
| **D-P2-08 / O5** | USB power path (B-P2-09) | A: ideal-diode OR-ing; B: source-select link, never both; C: USB powers logic only through an open-by-default jumper | **B** in Rev A: a 0 Ω link or solder jumper, USB data always connected, VBUS kept off the power path when the battery powers the board. A only if it fits the mass and area budget. Must satisfy the four constraints in §3.5 of the blocks (no VBUS to the motor rail, no battery back-feed, no series diode on the battery logic path, USB data plus battery motors in Phase 3) |
| **D-P2-04 / B-P2-11** | IMU isolation with the board screwed down | (a) Board-level soft mount on silicone grommets or TPU washers on the posts; (b) IMU on a slotted island; (c) IMU on a separate carrier on foam; (d) hard mount with filtering only | **(a)**, with the IMU at the centre of gravity over solid ground, plus six unpopulated pads for an off-board IMU as a fallback. An island on a board this small is unlikely to isolate motor frequencies, so do not rely on it. Soft mount means grommet clearance in the hole and ring geometry (see C3, C4) |
| **D-P2-03 / B-P2-14** | USB-C edge treatment | Widen the board locally on the aircraft-left edge to reach the wall; keep the OEM outline and cut a wall opening | **Keep the outline and cut a wall opening** for the whole plug body, if the wall distance (re-checked in C1) allows. Widening costs mass and moves the outline away from the measured seat. Opening size from the plug datasheet (verify) plus a clearance |
| **D-P2-06** | ToF | Breakout on a connector (Rev A); chip on board | **Breakout on a 6-way connector** (3V3, GND, SDA, SCL, XSHUT, optional GPIO1), cable as short as the floor opening allows, on I2C1. The breakout already has 10 kΩ pull-ups (§2.2). Position follows the floor-hole location, which the user measures |
| **D-P2-10 (new)** | Layer count, board thickness, copper weight | 2 or 4 layers; 0.8 mm or 1.6 mm; 1 oz or 2 oz | **4 layers** if the fab offers it at a reasonable price (solid ground under the IMU and I²C, power pours without starving the ground) and **0.8 mm** thickness if the connector and module allow it. **1 oz** with pours on both outer layers for the battery path; 2 oz only if the calculation fails. Record the fab's thickness and copper options |
| **D-P2-11 (new)** | Antenna edge and module position | Nose edge, tail edge, aircraft-right edge | **Not on the USB-C (left) edge and not facing the battery.** Fix only the edge now, from C1. The exact keep-out is placed in P2-7 from the module datasheet (verify) |
| **D-P2-12 (new)** | Shared battery path and fuse location | Fuse on the board, fuse in the lead only, or a footprint that takes either | **A fuse footprint on the board** after the battery connector and before the reverse-polarity P-FET, populated for bring-up and replaceable by a 0 Ω link in flight if the pack carries its own protection. Size the copper for the running current of all four motors (6.0 A measured) with stall as a short transient the fuse covers. The calculation uses the time-current curve of a real part (verify against the fuse datasheet) |
| **D-P2-13 (new)** | Fuse values | Bench fuse, board fuse | Keep **both figures with their purpose**: the inline 5 to 7.5 A bench fuse of the blueprint for first tests (one motor at a time, highest stall 2.95 A), and the board or aircraft fuse sized in D-P2-12 |
| **D-P2-05** | Camera | Mechanical only | Already decided in P2-1 (B-P2-06). Confirm only |

## 2. Analyses Claude Code produces (each goes into `pcb/specs/requirements.md`, with the formulas and tags)

**A. Mass budget.** A table of every item with mass and tag, weighed values where C5 is done, `[assumed]` where not. Target for the new board mass from the OEM board mass as the starting reference, proposed by Claude Code with its reason. Thrust-to-weight cannot be computed until the Phase 3 thrust data exists, so give the formula only:

$$\mathrm{TWR} = \frac{\sum_i T_{i,\max}}{m_{\mathrm{AUW}}\,g}$$

and state the required thrust for a TWR target (tagged `[assumed]`) so Phase 4 can check it.

**B. IMU placement rules.** The centre-of-gravity position from the mass distribution (estimated now, measured when the aircraft is weighed and balanced). Rules with a reason each: minimum distances to the antenna, LDO, module and MOSFET and motor traces; solid ground under and around the IMU; no copper pour or high-current return passing under it; thermal separation from the LDO and module; mounting through the soft mount. Every distance is `[assumed]` until Phase 3 shows the noise, and is listed as such.

**C. USB-C fit and mass.** Connector candidates from the JLCPCB database whose datasheet shows D+ and D− (the 6-pin power-only part is excluded). Height, length, mass and plug overmould size, each from the datasheet (verify). The wall opening size. Go or no-go against the cavity height measured in P2-1 (B-P2-12).

**D. Power path and LDO analysis.**
- LDO dissipation on USB: $P = (V_{\mathrm{in}} - 3.3\,\mathrm{V})\,I_{\mathrm{load}}$ at 5 V and at the worst-case load current from the module datasheet (Wi-Fi transmit peak, verify), plus the IMU and the ToF. Temperature rise from the AP2112K thermal resistance (verify against the datasheet).
- Battery path: the 3.55 V minimum input at full load (blueprint §3.1) and the battery sag under the measured 6.0 A running load. State the battery voltage at which the logic rail drops out.
- Check the chosen option against the four constraints and against R2 (brownouts seen on the breadboard).

**E. Module comparison.** A table for the shortlisted variants: mass, size, reserved GPIOs on the module, flash and PSRAM type, stock and price, §4.9 options to re-verify.

**F. Package and assembly table.** Every planned IC and active part with package, hand-solder feasibility, and fab-assembly availability from the database. The recommended assembly split.

**G. Mass of the board and the stack-up.** Estimated bare-board mass for the chosen thickness and layer count from the outline area and the fab's data (tagged), so the mass budget in A can be checked.

## 3. Procedure

1. Run Prompt 1. Resolve C1 to C6 with the user.
2. Run Prompt 2. One decision at a time, in the order of §1, with the analyses from §2 shown first.
3. Run Prompt 3 to write `requirements.md` and the IMU rules.
4. Run Prompt 4 to close out and update the blueprint.

## 4. Prompts for Claude Code (paste one at a time, in a new session started from the repo root with the `kicad` server connected)

**Prompt 1: carry-in items**

> Read `docs/P2-2_package.md`, `docs/P2-1_package.md` (§7 and §8), `pcb/specs/motor_electrical.md`, `pcb/mechanical/measurements.md`, `pcb/mechanical/constraints.csv` and `pcb/decision_log.md`. Work through the carry-in items C1 to C6 in §0 of the P2-2 package, one at a time. For each: say what it is, what you need from me, and wait. Update `constraints.csv` and `measurements.md` with my answers and tags. For C3, read the fab's capability page (the one for the fab named in the project, otherwise ask me which), record each figure with its URL, date and tag in a draft `pcb/specs/design_rules.md`, and tell me whether the current mounting holes and outline are manufacturable. Do not run KiCad design edits. Commit as "P2-2: carry-in items" and stop.

**Prompt 2: decision session**

> Run the decision session for P2-2. Take the decisions in §1 of `docs/P2-2_package.md` one at a time in the table's order. For each: (1) state the decision and the options, (2) show the analysis from §2 that applies, with formulas and tags, (3) give your recommendation and the reason, (4) ask me, and wait. Record my answer, not your recommendation, in `pcb/decision_log.md` (date, ID, decision, reason, state: decided, deferred with the trigger, or rejected). If I cannot decide, defer it with the measurement or document needed, and do not guess. Ask no more than one question per message. For anything recalled from memory, write "verify against datasheet" and add it to a list. Do not touch the schematic or the PCB file.

**Prompt 3: requirements and rules**

> Write `pcb/specs/requirements.md` from the decisions recorded in `decision_log.md` and the analyses in §2 of `docs/P2-2_package.md`: mass budget (A), IMU placement rules (B), USB-C fit (C), power path and LDO analysis (D), module comparison (E), package and assembly table (F), board mass and stack-up (G). Every number tagged `[measured]`, `[datasheet p.N]` or `[assumed]`, and list all `[assumed]` and all "verify" items at the end. Keep a short list at the top of what P2-3 (datasheet library) must collect: every part and document named in the decisions. Show me the file and wait for my comments before committing.

**Prompt 4: close-out and blueprint**

> Close out P2-2. (1) Fill §8 Results of `docs/P2-2_package.md` with the gate checks from §5 and set the status line. (2) Update the diff table in §7: mark every row Applied, Pending, Deferred or Rejected with a reason, and add any new diff using the next free B-P2 number (B-P2-19 onwards), in the format ID, section, old, new, reason, state. Show me the diffs and wait for approval before touching the blueprint. (3) Apply the approved diffs to the blueprint (revision 2.15 unless I say otherwise) and add a dated row per diff to `pcb/decision_log.md`. (4) Update the decision table in the blueprint's Phase 2 section (state of D-P2-02 to D-P2-13), O3, O5, O6, R2, R10 and R11 where the decisions change them, and update the Phase 2 status line and the P2-2 gate row. (5) List every remaining `[assumed]` item, every open question, the datasheet and document list for P2-3, and the questions you would ask at the start of P2-3. Do not start P2-3.

## 5. Acceptance criteria (the gate)

1. C1 to C6 resolved or explicitly deferred with a named trigger.
2. Every decision in §1 is decided, deferred with its trigger, or rejected, and each answer is recorded in `decision_log.md` with the user's own words as the reason.
3. `requirements.md` contains analyses A to G with tagged numbers, and a list of `[assumed]` and "verify" items.
4. The USB power path satisfies the four constraints, and the LDO dissipation and dropout calculations are shown (D-P2-08).
5. The IMU placement rules and the isolation decision are written down (D-P2-04).
6. A USB-C connector candidate with D+ and D− is named with a go or no-go (D-P2-03).
7. The fab's capability figures are recorded and the mounting holes and outline are confirmed manufacturable, or a change is proposed (C3).
8. The mass budget is based on weighed values, or is clearly marked as assumed with a deadline (before P2-9).
9. The user approves the decision log, and the blueprint reflects the approved diffs. No KiCad design edits were made.

## 6. Notes for later stages (do not act on them now)

- P2-3 collects the datasheets for everything named in the decisions, plus the ESP32-S3 hardware design guidelines and the module datasheet.
- Pending diffs B-P2-04 and B-P2-05 (module pad column, closing O1) belong to P2-4.
- If the soft-mount option is chosen, the grommet or washer design is a SOLIDWORKS task for P2-7 and Phase 4, not P2-2.

## 7. Blueprint diffs

| ID | Section | Old | New | Reason | State |
|---|---|---|---|---|---|
| B-P2-09 | §3.1, §3.5 | USB power path open | The path chosen for D-P2-08, with the LDO and dropout analysis | D-P2-08 | Pending |
| B-P2-11 | §3.3 | "Soft-mount the IMU (foam tape, grommets or TPU mount)" | The isolation option chosen for D-P2-04 and the board-level mount geometry | D-P2-04 | Pending |
| B-P2-14 | §5 Phase 2 (D-P2-03), R10 | USB-C go from P2-1 (B-P2-12, applied) | Edge treatment (opening or widening) from D-P2-03 | D-P2-03 | Pending |
| B-P2-15 | §5 Phase 2 (D-P2-07) | Hand-solder preference (B-P2-13, applied) | Assembly method chosen for D-P2-07 | D-P2-07 | Pending |
| B-P2-16 | §5 Phase 2 | Layer count open | Stack-up, thickness, copper (D-P2-10) | D-P2-10 | Pending |
| B-P2-17 | §3.1, §3.2 | One fuse figure (5 to 7.5 A bench) | Bench fuse and board fuse with their purposes, fuse footprint and location (D-P2-12, D-P2-13) | The P2-1 figure of about 10 A answers a different question | Pending |
| B-P2-18 | §6.1 | O3, O5, O6 open | Closed or deferred with the trigger | P2-2 decisions | Pending |

Claude Code adds rows as P2-2 uncovers conflicts, from B-P2-19. Numbering note (2026-11-20): D-P2-09 is the outline decision from P2-1, so the new decisions are D-P2-10 to D-P2-13; B-P2-12 and B-P2-13 were applied at the P2-1 gate, so their P2-2 follow-ups are B-P2-14 and B-P2-15.

## 8. Results

*Filled in by Claude Code at close-out (Prompt 4).*

| Check | Result |
|---|---|
| Carry-in items C1 to C6 | |
| Decisions closed or deferred | |
| `requirements.md` analyses A to G | |
| USB power path and LDO analysis | |
| IMU rules and isolation | |
| USB-C go or no-go | |
| Fab capability, outline manufacturable | |
| Mass budget (weighed or assumed) | |
| User approval, blueprint at revision | |

## 9. Files

```
pcb/specs/requirements.md        (new)
pcb/specs/design_rules.md        (draft: fab figures)
pcb/decision_log.md              (updated)
pcb/mechanical/constraints.csv, measurements.md   (corrected)
docs/P2-2_package.md
```

Also changed: the blueprint (decision table, O3, O5, O6, R2, R10, R11, status line, P2-2 gate row).
