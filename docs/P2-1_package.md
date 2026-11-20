# Franken-E58 Phase 2, Stage P2-1: Mechanical capture, motor current, folder and rules

**Scope:** set up the `pcb/` folder and the PCB working rules; capture every mechanical fact the board must fit (OEM board, frame cavity, connectors, camera connector, mass); measure the OEM motors' stall current (blueprint diff B-P2-02); turn the measurements into a constraints sheet and a board outline; and apply the agreed Phase 2 blueprint blocks. No schematic, no part selection and no KiCad design work in P2-1.

Blueprint sections used: §2.1, §3.1, §3.2, §4.10, Phase 2 (revision 2.15 blocks), Phase 3 step 2, O1 to O4. Also read `docs/PHASE1_REPORT.md` (Phase 2 inputs) and `docs/Phase2_blueprint_blocks.md`.

**Status: P2-1 complete, gate passed 2026-11-20** (deviations: one stall reading per motor and no fuse, see §8). Next: P2-2.

**Credentials:** none.

**Hard rules for this stage**
- **Never guess a dimension.** Every number is tagged `[measured]` (with the tool and date), `[datasheet p.N]` or `[assumed]`. A missing measurement is asked for, one group at a time.
- Props off, no firmware flashing. The only live-power work is the stall-current measurement (§4, step 6).
- **Battery safety for that measurement:** protected high-drain 18650 (25R/30Q/VTC class), inline 5 to 7.5 A fuse, multimeter on the 10 A range in series, 1 to 2 seconds per reading, attended, nothing flammable nearby, no salvaged cell, charge only on a proper charger off the aircraft.
- British English. Do not touch the firmware tree.

---

## 1. Spec

### Part 0: folder, rules and housekeeping
- Create the `pcb/` tree exactly as in Block 2 of the Phase 2 blocks. Add a `README.md` (status, stage table, how to open the project) and `decision_log.md` (date, decision, reason).
- `.gitignore`: `pcb/datasheets/*.pdf`, `pcb/outputs/*.zip`, `pcb/kicad/**/*-backups/`, `pcb/kicad/**/fp-info-cache`, `pcb/mechanical/*.sldprt`, `*.sldasm` (large binaries stay local; keep the STEP and DXF tracked).
- `CLAUDE.md`: add a **PCB section** with Block 5 of the Phase 2 blocks (datasheet first, tagged numbers, independent verification, global power symbols, git discipline, no autorouting of the motor and IMU areas, no guessing). Status line set to P2-1.
- `.claude/settings.json`: allow read-only KiCad MCP tools and `kicad-cli` exports. Deny anything that writes outside `pcb/` and deny firmware commands (`pio run -t upload`, `uploadfs`, the monitor).
- Create `pcb/mechanical/measurements.md` from the checklist in §2 and a CSV `pcb/mechanical/constraints.csv` (`id, item, x_mm, y_mm, size_mm, tolerance_mm, tag, note`).

### Part 1: measurement session
Claude Code runs it as an interview, one group at a time, records each answer into `measurements.md` with its tag, and asks for a photo where a number is doubtful. All coordinates are from **one datum**: the lower-left corner of the OEM board seen from the top, x towards the front of the aircraft's right-hand side, y towards the nose (state the orientation in the file and keep it).

### Part 2: derived data
- `pcb/specs/motor_electrical.md`: stall current per motor, running current if measured, derived limits (§2, group G).
- `pcb/mechanical/constraints.csv` finished and signed off.
- `pcb/mechanical/outline.dxf` (from SOLIDWORKS) and `pcb/mechanical/cavity.step` (cavity envelope, from SOLIDWORKS), and `pcb/tools/check_outline.py` that validates the DXF (units millimetres, one closed outline, bounding box within 0.1 mm of the measured values, hole positions and diameters against the CSV).

### Part 3: first KiCad project shell
Create `pcb/kicad/franken-e58/` with an empty project, the board outline on Edge.Cuts from the validated DXF (or from the constraints CSV if the MCP server has no DXF import; check the available tools first), mounting holes at the measured positions, and keep-out areas for the height limits. Commit. No parts yet.

## 2. Measurement checklist (the interview)

Ask first, before anything else: **do you have digital calipers?** The blueprint assumes only a multimeter. Fall back to a steel ruler plus top-down photos with a scale in frame (accuracy about ±0.5 mm), and record the tool used in every row.

| Group | Items to record |
|---|---|
| **A. OEM board** | Outline (length, width, thickness, corner radii, notches or cut-outs), copper-free zones, mass, photos of both sides with a scale |
| **B. Mounting** | Screw posts and holes: positions from the datum, hole diameter, post outer diameter, post height, screw type and length, whether posts are on the frame or the board |
| **C. Frame cavity** (the frame decides the fit, not the OEM board) | Internal length and width at the seat, wall positions, height available under the canopy, clearances to the battery bay and motor arms, any ribs or posts that intrude |
| **D. Battery** | Connector type, pitch, polarity, wire gauge, position and exit direction. Cell dimensions and mass |
| **E. Motors** | Four motor connections: plug or solder pads, connector type, pitch, wire length, polarity convention, positions. Motor mass. Propeller direction of each arm (feeds σ, §4.5) |
| **F. Camera** | Where it connects, connector type (ZIF/FFC, pin count, pitch), module size and mass, cable length and route, the mount and the clearance it needs. **What the connector carries:** is there a video chip on the camera module or only a sensor? Which signals reach the OEM board (continuity check on a few pins with the multimeter, power off)? |
| **G. Motor electrical** | Per motor: stall current (§4, step 6), supply voltage used, wire resistance if easy to read. Used to size traces, the AO3400A, the SS34 and the fuse |
| **H. Other** | LEDs, switches, antenna wire, any item on the OEM board the new board must keep a place for, USB-C connector envelope check (height 3.2 mm class and length about 9 mm class, *verify against the connector datasheet in P2-3*) against the cavity height |
| **I. Mass budget** | OEM board, frame, four motors and props, battery, camera, wiring. Total flying mass if the aircraft is assembled |

## 3. Decisions to record or confirm (ask the user, record in `decision_log.md`)

| ID | Question | Default |
|---|---|---|
| D-P2-05 | Camera: mechanical only in Rev A? Decide after group F | Yes, mechanical only |
| D-P2-03 | Does a USB-C receptacle (height and edge position) fit the cavity, and can it sit at an edge reachable without removing the canopy? | Fit check from groups C and H |
| D-P2-07 | Hand-solder or fab assembly? (Do not decide here; note the preference for P2-2.) | Fab assembly of SMD parts |
| D-P2-04 | Is there a candidate position for the IMU at the centre of gravity inside the cavity? Measure the centre of gravity of the assembled aircraft if possible (balance on a pencil or string) | Geometric centre |

## 4. Procedure

1. **Open session.** Run Prompt 1 (§5). Confirm the folder tree, rules and `.gitignore` are in place.
2. **Photograph.** OEM board top and bottom, frame cavity from above and from the side, each connector close-up, camera connector close-up, always with a scale in frame. Save to `pcb/mechanical/photos/` using names like `oem_top.jpg`.
3. **Measure and interview.** Run Prompt 2 (§5). Answer group by group. Mark anything unsure and Claude Code will ask for a second reading or a photo.
4. **Camera inspection.** Group F, including the continuity check on a few pins (multimeter, power off). Record which signals reach the OEM board.
5. **SOLIDWORKS.** Model the frame cavity envelope and the board seat from the signed-off constraints. Export (a) the board outline as a DXF (millimetres, one closed profile, mounting holes as circles) to `pcb/mechanical/outline.dxf`, and (b) the cavity envelope as a STEP to `pcb/mechanical/cavity.step`. Keep the `.sldprt` local.
6. **Stall current (B-P2-02).** One motor at a time, **props off**, motor mounted or held so it cannot move, protected 18650 behind the 5 to 7.5 A fuse, multimeter on the 10 A range in series. Hold the shaft by clamping the motor body and the shaft only for 1 to 2 seconds, read the peak, release, let it cool for at least 30 seconds, repeat 3 times per motor. Record each reading in `motor_electrical.md`. If the multimeter fuse blows or a motor heats, stop and tell Claude Code. No battery charging on the bench during this step.
7. **Outline check.** Run `check_outline.py` (Prompt 3). Then **print the outline 1:1** (check the printer scale with a ruler), cut it out and lay it on the OEM board and in the frame cavity. It must sit on the screw posts and clear the walls by the intended margin.
8. **KiCad shell.** Run Prompt 4. Open the project in KiCad, check the outline and holes visually, and confirm the 3D viewer shows the outline.
9. **Close out.** Run Prompt 5.

## 5. Prompts for Claude Code (paste one at a time, in a new session started from the repo root with the `kicad` server connected)

**Prompt 1: folder, rules, housekeeping**

> Read `docs/Phase2_blueprint_blocks.md`, `docs/PHASE1_REPORT.md` (the Phase 2 inputs section) and `docs/P2-1_package.md`. Do Part 0 of the P2-1 spec: create the `pcb/` tree from Block 2, `pcb/README.md`, `pcb/decision_log.md`, the `.gitignore` entries, the PCB section in `CLAUDE.md` (Block 5 rules, status line P2-1), and the `.claude/settings.json` permissions (allow read-only KiCad MCP tools and `kicad-cli` exports, deny writes outside `pcb/`, deny firmware upload, uploadfs and the monitor). Do not touch `src/`, `components/` or `platformio.ini`. Create `pcb/mechanical/measurements.md` from the checklist in §2 and `pcb/mechanical/constraints.csv` with the header only. Commit to git as "P2-1: pcb folder and rules". Then list what you changed and stop.

**Prompt 2: measurement interview**

> Run the P2-1 measurement interview from §2 of `docs/P2-1_package.md`. First ask only whether I have digital calipers. Then take groups A to I one at a time: ask for the items in the group, record each answer in `pcb/mechanical/measurements.md` with its tag (`[measured]` with tool, `[datasheet p.N]` or `[assumed]`), and add the dimensioned items to `pcb/mechanical/constraints.csv`. Never guess a number. If an answer looks implausible (for example a hole spacing that does not fit inside the stated board width), say so and ask again. After group F, summarise what the camera connector carries and propose the wording for decision D-P2-05, and wait for my answer. After group I, show me the finished constraints table and the mass budget and wait for my sign-off before you save the "signed off" mark.

**Prompt 3: outline check and stall-current record**

> Do two things. (1) Write `pcb/tools/check_outline.py` (Python, `ezdxf`, installed in a venv inside `pcb/tools/`, never system-wide): it reads `pcb/mechanical/outline.dxf`, checks units are millimetres, exactly one closed outline, the bounding box within 0.1 mm of the board length and width in `constraints.csv`, and that every mounting hole's position and diameter match the CSV within 0.1 mm. It prints one PASS or FAIL block. Run it and show me the result. (2) Create `pcb/specs/motor_electrical.md` from my recorded stall-current readings: a table of all readings per motor, the highest peak, and these derived limits with their formulas: external trace width for the highest peak at a 10 °C rise using IPC-2221, $I = 0.048\,\Delta T^{0.44}A^{0.725}$ with A in mil² (label it "verify against IPC-2221"), the margin against the AO3400A continuous rating and the SS34 3 A rating, and a fuse suggestion. Label every datasheet figure "verify against datasheet" until it is checked in P2-3.

**Prompt 4: KiCad project shell**

> Using the `kicad` MCP tools, create the empty project `pcb/kicad/franken-e58/franken-e58.kicad_pro`. First list the available tools and tell me whether one can import a DXF outline. If yes, import `pcb/mechanical/outline.dxf` onto Edge.Cuts. If not, draw the outline from `constraints.csv`. Place the mounting holes at the measured positions and add keep-out zones for the height limits recorded in group C. Run DRC through the MCP server and through `kicad-cli`, save a top view and a 3D render to `pcb/outputs/`, commit as "P2-1: KiCad project shell", and show me the renders. No parts, no schematic.

**Prompt 5: close-out and blueprint**

> Close out P2-1. (1) Fill §8 Results of `docs/P2-1_package.md` with the gate checks from §6 and the measured numbers, and set the Status line. (2) Update the diff table in §7: mark each row Applied, Pending or Rejected with a reason, and add any new diff you found, using the format ID, section, old, new, reason, state. Never edit the blueprint silently: show me the diff first. (3) Apply the approved diffs to the blueprint (revision 2.15) and write the date and reason in `decision_log.md`. (4) List every assumption still tagged `[assumed]`, every open question, and your recommended next questions for P2-2. Do not start P2-2.

## 6. Acceptance criteria (the gate)
1. The `pcb/` tree, `README.md`, `decision_log.md`, `.gitignore` entries, the `CLAUDE.md` PCB section and the permissions exist. The firmware tree is unchanged (`git diff` outside `pcb/`, `docs/` and the two config files is empty).
2. `measurements.md` covers groups A to I, every number tagged, and the user has signed off `constraints.csv`.
3. The camera connector is documented: type, pin count, pitch, what reaches the OEM board. D-P2-05 is recorded.
4. `motor_electrical.md` has at least 3 stall readings per motor, with the highest peak and the derived limits.
5. `check_outline.py` reports PASS (bounding box and holes within 0.1 mm).
6. **The 1:1 printout** sits on the screw posts and clears the cavity walls by the intended margin.
7. The KiCad shell opens in KiCad with the outline and holes in the right place, DRC reports 0 errors in the MCP server and in `kicad-cli`, and the 3D viewer shows the outline.
8. A mass budget exists, and a USB-C height and edge-position check (D-P2-03) is recorded with a go or no-go for P2-2.
9. All proposed diffs are marked Applied, Pending or Rejected, and the blueprint is at revision 2.15.

## 7. Blueprint diffs

| ID | Section | Old | New | Reason | State |
|---|---|---|---|---|---|
| B-P2-01 | §4.10 | No PCB folder | Add `pcb/` | Dedicated Phase 2 folder | Applied (revision 2.15, §4.10: `pcb/` in the repository layout) |
| B-P2-02 | §6.1 O2, Phase 3 step 2 | Stall current measured in Phase 3 | Measured in P2-1 on the OEM motors, re-measured in Phase 3 | Sizing of traces and parts | Applied 2026-11-20: §2.1 Motors row, O2, O8 closed, Phase 3 step 2 (three readings through the fuse on the final board) |
| B-P2-03 | Phase 2 step 1 | No programming or boot circuit | Add EN circuit, BOOT, USB-C, CC resistors, ESD, UART0 pads, test points | Bare module needs them | Applied (revision 2.15: Phase 2 step 1 and §3.5) |
| B-P2-04 | §3.4 | Dev-board pin list | Module pad column, module reserved GPIOs, USB on GPIO 19 and 20 | Module breaks out fewer pins | Pending (P2-4): needs the module datasheet pad table (P2-3) |
| B-P2-05 | §6.1 O1 | Open | Close as not applicable | No dev-board LED on the module | Pending (P2-4, with B-P2-04): O1 stays open in the blueprint until the pin map is re-derived |
| B-P2-06 | §2.1 | "Camera retained" | Rev A mechanical only | Pending the group F inspection | Applied 2026-11-20: intro, §2.1 Frame row, D-P2-05 Decided, O7 closed (group F inspection) |
| B-P2-07 | Phase 2 | "Shield board" | Single flight-controller board | D-P2-01 | Applied (revision 2.15: Phase 2 objective) |
| B-P2-08 | Phase 2 step 3 | Fixed 40 to 50 mil | Width calculated from the stall current | Real current | Applied (revision 2.15: Phase 2 step 3); widths in `pcb/specs/motor_electrical.md` |
| B-P2-09 | §3.1 | No USB power path | Add the path chosen in P2-2 (Block 4) | D-P2-08 | Pending (P2-2) |
| B-P2-10 | §6.1, §6.2 | O1 to O4, R1 to R8 | Add O5 to O8, R9 to R11 | Phase 2 findings | Applied (revision 2.15: O5 to O8, R9 to R11) |
| B-P2-11 | §3.3 (mechanical constraint), D-P2-04 | "Whole board soft-mounted" (D-P2-04); §3.3 asks for the IMU to be soft-mounted | Board screwed rigidly to the four frame posts. The §3.3 IMU soft mount is met another way, chosen in P2-2 (isolated IMU island using the reserved slot or cut-out option, or a separate IMU carrier), with the DLPF and notch filters as the fallback (R1) | User, 2026-11-20: board stability first. The 2.0 mm holes leave a 0.2 mm rim, so grommets cannot fit the OEM hole pattern anyway | Pending (P2-2) |
| B-P2-12 | §5 Phase 2 D-P2-03, §6.2 R10 | "Mass and height checked in P2-1"; R10 "checked in P2-1" | P2-1 fit check: go. Aircraft-left edge, y = 3 to 13 mm, flush, through a frame wall opening for the whole plug (wall about 6 mm from the board edge); local board widening open for P2-2 | Groups C and H, SOLIDWORKS model | Applied 2026-11-20 |
| B-P2-13 | §5 Phase 2 D-P2-07 | "Default fab assembly of SMD parts" | Preference recorded: the user hand-solders (passives 0603 or larger, no LGA, QFN only with a hot-air plan); closed in P2-2 | User, P2-1 interview 2026-11-12 | Applied 2026-11-20 |

Claude Code adds rows here as P2-1 uncovers conflicts, for example if the cavity cannot take a USB-C receptacle, the OEM board has features the blocks missed, or the measured stall current exceeds the AO3400A rating.

## 8. Results

Closed out 2026-11-20 (Prompt 5). Gate items from §6 in brackets.

| Check | Result |
|---|---|
| Folder, rules, permissions (1) | **Pass.** `pcb/` tree, README, decision log, `.gitignore`, CLAUDE.md PCB section and permissions in place. The stray copy `tools/check_outline.py` (committed in `3fc681a`) was removed by the user on 2026-11-20; after that commit, nothing outside `pcb/`, `docs/`, `CLAUDE.md` and `.claude/settings.json` differs from the start of P2-1 |
| Measurements A to I, sign-off (2) | **Pass.** Signed off 2026-11-12; re-signed 2026-11-20 after the 1:1 printout and the SOLIDWORKS model (`outline.dxf`, `cavity.step`). Every number tagged |
| Camera connector (3) | **Pass.** 20-pin FPC cameras on the OEM camera adapter (Beken BK7231 SoC); only a 3-wire lead (V+, GND, one-way serial) reaches the flight board. D-P2-05 Decided: mechanical only |
| Stall current per motor (peak) (4) | **Pass with deviation.** M1 2.92, M2 2.93, M3 2.93, M4 2.95 A (DT9205A 20 A range, flight pack at 3.88 V). Highest 2.95 A; design value 3.5 A. Running with the prop on: 1.45 to 1.57 A, 6.0 A for all four. Trace widths at a 10 °C rise on 1 oz copper: 1.34 mm (2.95 A), 1.69 mm (3.5 A). **Deviation:** one reading per motor, not three; no fuse; repeats in Phase 3 step 2. `pcb/specs/motor_electrical.md` |
| `check_outline.py` (5) | **Pass** on `outline.dxf` (board 23.5 × 47.0, holes B1 to B4 within 0.1 mm) and on the Edge.Cuts DXF exported back out of KiCad (`pcb/outputs/p2-1_edge_cuts.dxf`) |
| 1:1 printout fit (6) | **Pass.** Printout screwed to the four posts and resting on the peg fins (video `pcb/photos/MAH00217.MP4`). Wall gap 1.3 mm at the nose and about 5 mm at the U-slots [measured: user, 2026-11-20], against 1.3 and 4.5 in the model. The tail edge lies flat on the seat with no rib or lip. Nothing latches the board. The pegs touch the paper slot edges with no visible clearance (model: about 0.8 mm radial). The user keeps the 4.2 mm slots and will scrape them if the real board binds |
| KiCad shell, DRC, 3D (7) | **Pass.** `pcb/kicad/franken-e58/`: outline (10 lines, 2 arcs), four 2.0 mm NPTH holes, 11 keep-out rule areas (K1 to K3), height notes, board 1.0 mm. DRC: 0 errors in both the MCP server and `kicad-cli`, 4 accepted `lib_footprint_mismatch` warnings (MCP-generated hole footprint). Renders: `pcb/outputs/p2-1_top.png`, `p2-1_3d.png`, `p2-1_keepouts.png`. Outline, holes and 3D view checked by the user in KiCad, 2026-11-20 |
| Mass budget (8) | **Pass (all values assumed).** 85 g flying mass, 98 g with a 15 % factor; new board at most 6 g. No scale available: weigh before P2-9 |
| USB-C go or no-go (8) | **Go.** Aircraft-left edge, y = 3 to 13 mm, flush. The frame wall there is about 6 mm out, so the wall opening takes the whole plug overmould. Local board widening is an option for P2-2 (B-P2-12) |
| Diffs and blueprint (9) | **Pass.** Every row in §7 is Applied or Pending. Blueprint at revision 2.15 with B-P2-02, 06, 12 and 13 applied on 2026-11-20 |

## 9. Files

```
pcb/README.md, pcb/decision_log.md
pcb/mechanical/       measurements.md, constraints.csv, outline.dxf, cavity.step, photos/
pcb/specs/            motor_electrical.md
pcb/tools/            check_outline.py (venv inside pcb/tools/)
pcb/kicad/franken-e58/  franken-e58.kicad_pro, .kicad_pcb (outline and holes only)
pcb/outputs/          top and 3D renders
docs/P2-1_package.md
```

Also changed: `.gitignore`, `CLAUDE.md`, `.claude/settings.json`, the blueprint (revision 2.15).
