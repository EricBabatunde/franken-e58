# Franken-E58 Phase 2: blueprint blocks (proposed revision 2.15)

**How to use this file.** Each block below says where it goes in the blueprint. Paste them, bump the revision to 2.15, and keep the diff table (Block 7) in the blueprint until every row is `Applied`. IDs `B-P2-nn` replace the draft IDs D1 to D6 used in chat. Claude Code reads the updated blueprint and works from it. Every number marked *verify* is recalled from memory and must be checked against the datasheet before it is used.

---

## Block 1: replaces the heading, objective and step list of §5 Phase 2

### Phase 2: Schematic Capture & PCB Design

**Objective:** one compact, light, 2- or 4-layer **flight-controller board** that carries the ESP32-S3 module, IMU, ToF connection, motor drivers, power stage and USB programming port, and fits the E58 frame cavity. The old shield-board wording is retired (decision D-P2-01).

**Status: P2-0 complete (toolchain), P2-1 next.** Package files: `docs/P2-0_package.md`, `docs/P2-1_package.md`.

**Design decisions so far**

| ID | Decision | Reason | State |
|---|---|---|---|
| D-P2-01 | **Single PCB** with the ESP32-S3 module soldered on. No dev board, no shield | A 44-pin dev board is too tall and heavy for a micro-quad cavity. Mass and height are the main constraints | Decided |
| D-P2-02 | Module: **ESP32-S3-WROOM-1-N16R8** by default | It matches the module family validated in Phase 1 (flash and PSRAM modes, `sdkconfig`). Any other module (N8, MINI) needs the §4.9 options re-verified. Closed in P2-2 (O3) | Default, open |
| D-P2-03 | **USB-C receptacle with D+ and D−** on GPIO 20 and 19 (native USB Serial/JTAG) is the programming, logging and debug port. Plus BOOT and EN buttons and UART0 pads as the fallback (Block 4) | Same native port as Phase 1, no USB-UART bridge needed | Decided in principle. Mass and height checked in P2-1, power path in P2-2 |
| D-P2-04 | **IMU at the geometric centre and centre of gravity**, away from the antenna, LDO, module and motor currents, over solid ground, whole board soft-mounted | Least noise (R1) | Decided. Slot or cut-out option reserved |
| D-P2-05 | **Camera: mechanical only in Rev A** (mount and cable clearance kept, no electrical connection) | The video and Wi-Fi chip is probably on the OEM board, and a DVP camera needs about 12 GPIOs | Pending the P2-1 inspection of the camera connector |
| D-P2-06 | **ToF: breakout on a connector** in Rev A, chip on board later | The VL53L1X is a small optical LGA that needs a cover window | Proposed, confirm in P2-2 |
| D-P2-07 | Assembly method (hand-solder versus fab assembly) | Decides package choices (LGA, QFN, passive size) | **Open**, default fab assembly of SMD parts. Closed in P2-2 |
| D-P2-08 | Power path between USB 5 V and the 1S battery | Phase 3 needs USB logging while the motors run from the battery. VBUS must never back-feed the battery or reach the motor rail | **Open**, closed in P2-2 (Block 4) |

**Steps**

1. **Schematic.** AP2112K with 10 µF MLCC + 100 nF at input and output plus the bulk polymer capacitor; four AO3400A switches with 47 Ω gate resistors and 10 kΩ pull-downs; SS34 flyback diodes; 100 nF motor capacitors; two I²C buses with pull-ups sized per §2.2; VBAT divider; reverse-polarity P-FET footprint; fuse and battery connector; INA219/INA226 footprint (optional); **ESP32-S3 module minimum system, EN reset circuit, BOOT button, USB-C port with CC resistors and ESD protection, UART0 pads, test points and 0 Ω links on every motor line** (B-P2-03).
2. **Placement.** Module and MPU-6500 at the centre. MOSFETs and battery terminals on the periphery. **Keep the Wi-Fi antenna free of copper and ground pours** (module datasheet keep-out, *verify*).
3. **Routing.** Motor supply trace widths **calculated from the measured stall current** (B-P2-08). I²C traces 8 to 10 mil over solid ground. **Star ground** separating motor returns from logic ground.
4. **Module variant and mass.** Close O3. Check total board mass against the OEM board and the flight pack.
5. **Fit check.** Export a STEP from KiCad and check it against the SOLIDWORKS cavity model (P2-8).

---

## Block 2: adds to §4.10 Repository Layout

```
franken-e58/
└── pcb/                          (Phase 2 only; the firmware tree is not touched)
    ├── README.md                 status, stage table, how to open the project
    ├── decision_log.md           every design choice, with date and reason
    ├── mechanical/               photos, measurements.md, constraints.csv,
    │                             SOLIDWORKS model, outline.dxf, cavity.step
    ├── datasheets/               PDFs (git-ignored) and MANIFEST.md
    │                             (source URL, revision, date, SHA-256, page notes)
    ├── specs/                    requirements.md, motor_electrical.md,
    │                             net_table.csv, pin_map.csv, design_rules.md
    ├── reviews/                  one reference-design review per chip
    ├── kicad/
    │   ├── franken-e58/          the KiCad project
    │   └── lib/                  project symbol and footprint libraries
    ├── tools/                    outline check, netlist comparison, BOM and stock check
    └── outputs/                  Gerbers, drill, BOM, pick-and-place, renders, STEP
```

Docs for each stage go in `docs/` as `P2-n_package.md`, as in Phase 1.

---

## Block 3: Phase 2 stage plan and gates (replaces nothing; goes under the Phase 2 steps)

Each stage ends at a gate. Nothing moves on until the gate passes and the user signs off. Commit to git at every gate.

| Stage                          | Work                                                                                                                                                                                                                  | Gate (acceptance)                                                                                                                                                                          |
| ------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **P2-0** Toolchain             | KiCad installed natively, KiCad MCP server connected to Claude Code, JLCPCB parts database, test board                                                                                                                | `claude mcp list` shows `kicad` connected. Test board passes ERC and DRC and exports. **Passed 11 November 2026**                                                                          |
| **P2-1** Mechanical capture    | Measure the OEM board and the frame cavity. Camera and connector inspection. Motor stall current. Mass. SOLIDWORKS model and outline DXF. Folder and rules set up                                                     | Constraints sheet signed off. Outline imports at the right scale (bounding box within 0.1 mm of the measured value). A 1:1 printout fits the real cavity. Stall current recorded per motor |
| **P2-2** Architecture decision | Close D-P2-02, 05, 06, 07, 08 and O3. Mass budget. IMU placement rules. USB-C height and mass check                                                                                                                   | User approves the decision log. Blueprint updated                                                                                                                                          |
| **P2-3** Datasheet library     | Every IC and part, plus the ESP32-S3 hardware design guidelines and the module datasheet                                                                                                                              | `MANIFEST.md` complete: URL, revision, date, SHA-256 for every file                                                                                                                        |
| **P2-4** Specs                 | Net table (ref, part number, value, footprint, package, supplier number, stock, mass, datasheet), pin map (function, GPIO, module pad), design rules (trace widths and clearances from the fab and the stall current) | Every §3 part is in the net table. Pin map matches §3.4 as revised by B-P2-04. Stock checked                                                                                               |
| **P2-5** Schematic             | Reference-design review per chip first (datasheet circuit against ours, every difference listed with a page reference), then the schematic. Footprint land patterns checked against the manufacturer drawings         | Every difference justified or fixed. Power budget table (worst-case currents against the LDO rating and the 3.55 V dropout limit) done                                                     |
| **P2-6** ERC and netlist check | ERC, netlist export, independent script (not the generator) comparing every pin with the pin map and the datasheet pin tables                                                                                         | ERC clean. 100 % of pins match. Every unused pin has a declared treatment. No unexplained single-pin net                                                                                   |
| **P2-7** Placement             | IMU at centre, antenna keep-out, mounting holes and connectors from P2-1, power parts on the periphery                                                                                                                | User approves the placement render                                                                                                                                                         |
| **P2-8** Routing and DRC       | Motor traces, I²C over ground, star ground. DRC. STEP export and fit check in SOLIDWORKS                                                                                                                              | DRC clean (MCP and `kicad-cli`), no interference with the cavity model                                                                                                                     |
| **P2-9** Release               | Gerbers, drill, BOM, pick-and-place, final stock check, blueprint revision                                                                                                                                            | User signs off. First-power-up plan written (Block 4)                                                                                                                                      |

---

## Block 4: new §3.5 Programming, boot and debug path

**Port.** A USB-C receptacle that brings out **D+ and D−** (not the power-only 6-pin part used on the P2-0 test board), wired to the ESP32-S3 native USB Serial/JTAG: **GPIO 19 = D−, GPIO 20 = D+** (*verify against the ESP32-S3 datasheet*). It carries flashing, the log, the 1 kHz telemetry and JTAG debug, exactly as on the dev board in Phase 1. No USB-UART bridge is needed.

**Circuit (details fixed in P2-5 from the Espressif hardware design guidelines and the module datasheet):**

| Item | Purpose |
|---|---|
| 5.1 kΩ from CC1 and from CC2 to GND | Lets a USB-C host supply 5 V (as on the test board) |
| USB ESD protection array on D+ and D− | Protects GPIO 19 and 20 |
| Series resistors on D+ and D− only if the guidelines require them | *Verify* |
| EN pin: pull-up and capacitor to GND (module datasheet recommends an RC, typically 10 kΩ and 1 µF, *verify*) plus a reset button | Clean power-up and manual reset |
| BOOT button from GPIO 0 to GND (GPIO 0 is a strapping pin, §3.4) | Manual download mode |
| UART0 pads: TX GPIO 43, RX GPIO 44, GND, 3V3 | Fallback log and flashing path |
| Test points: 3V3, VBAT, GND, each motor gate | Bring-up |

**Normal flashing.** `pio run -t upload` over USB. The ROM bootloader on the native port normally resets and enters download mode without the buttons. If the board does not enumerate (blank or crashed firmware), hold BOOT, tap EN, release BOOT: it enumerates as `303a:1001`. Flash mode is re-verified on the first boot of the bare module, because the Phase 1 DIO-at-80-MHz setting came from the dev board's flash chip.

**Power path (open, D-P2-08).** Constraints:
1. VBUS (5 V) must never reach the motor rail, and must never back-feed the battery.
2. The battery must not be forced through a series diode on the logic path: the AP2112K already needs about 3.55 V in at full load (§3.1), and a Schottky drop would shrink the usable battery range further.
3. Phase 3 needs USB data **and** battery-powered motors at the same time.
4. A 5 V input into the AP2112K dissipates about $(5 - 3.3)\,\mathrm{V} \times I$ in a small package next to the IMU. At 300 mA that is about 0.5 W (estimate; thermal limit *verify against the datasheet*).

Options for P2-2: **(A)** ideal-diode OR-ing of USB and battery into the LDO input; **(B)** a jumper or header that selects one source, never both, with USB data still connected when the battery powers the board (VBUS left off the board's power path by a link); **(C)** USB powers the logic only through an open-by-default jumper for bench flashing. Default to B in Rev A unless A fits the mass and area budget.

**First power-up plan (written in P2-9):** USB only, battery and motors disconnected, props off. Check 3V3, the current draw and enumeration. Then flash the Phase 1 firmware and run its self-tests. Battery only after that: protected high-drain cell behind the 5 to 7.5 A inline fuse, attended.

---

## Block 5: new documentation-first rules for PCB work (add under §1 Operating principles or as §5 Phase 2 rules)

1. **Datasheet first.** No symbol, footprint or net is created from memory. The source (file and page) is recorded. Footprints are checked against the manufacturer land-pattern drawing, not only against the KiCad library.
2. **Every number carries a tag:** `[measured]`, `[datasheet p.N]` or `[assumed]`. Assumed values are listed in the stage report.
3. **Independent verification.** Netlist and pin checks run in a script that does not share code with whatever generated the schematic. ERC and DRC are run through both the MCP server and `kicad-cli`.
4. **Power nets use global power symbols**, never sheet-local labels (the P2-0 test board produced `/GND` against `GND` mismatches).
5. **Git discipline.** Commit before every gate and before any bulk MCP edit. The KiCad GUI is never open on the same file as the MCP server edits.
6. **No autorouting** for the motor and IMU areas.
7. **No guessing.** A missing measurement is asked for, not estimated.

---

## Block 6: additions to §6.1 Open Items and §6.2 Risk Register

| ID | Item | Resolution |
|---|---|---|
| O5 | USB power path (D-P2-08) | P2-2 |
| O6 | Assembly method (D-P2-07) | P2-2 |
| O7 | Camera treatment (D-P2-05) | Inspect the connector in P2-1 |
| O8 | OEM motor stall current | P2-1 measurement (B-P2-02) |

| ID | Risk | Mitigation |
|---|---|---|
| R9 | First-spin board errors (wrong footprint, pin swap, missing pull-up) | The P2-3 to P2-6 gates: datasheet review, footprint check, scripted pin-by-pin netlist comparison, test points and motor-line 0 Ω links for bring-up |
| R10 | USB-C connector mass, height or edge position does not fit the cavity | Checked in P2-1 against the cavity measurements. Fallback: UART0 and USB pads with a detachable cable |
| R11 | LDO heating near the IMU, especially on USB power | Power budget in P2-5. Keep the LDO and module away from the IMU. Revisit the regulator choice if needed |

---

## Block 7: blueprint diffs for Phase 2

| ID | Section | Old | New | Reason | State |
|---|---|---|---|---|---|
| B-P2-01 | §4.10 | No PCB folder | Add `pcb/` (Block 2) | Dedicated Phase 2 folder | Proposed |
| B-P2-02 | §6.1 O2, §5 Phase 3 step 2 | Stall current measured in Phase 3, after the PCB | Measure the OEM motors in P2-1. Phase 3 re-measures on the final board | Trace widths, the AO3400A (about 5.7 A continuous), the SS34 (3 A) and the fuse depend on it | Proposed |
| B-P2-03 | §5 Phase 2 step 1 | No programming or boot circuit in the schematic list | Add the EN circuit, BOOT button, USB-C port with D+/D−, CC resistors, ESD protection, UART0 pads, test points (Block 4) | Flashing and logging run over native USB (§4.1) and a bare module cannot be flashed without them | Proposed |
| B-P2-04 | §3.4 | Pin map and "Avoid" list written for the 44-pin dev board | Add a "module pad" column and re-derive the reserved GPIOs from the WROOM-1 pin table (*verify*). List GPIO 19 and 20 as USB in use | The module breaks out fewer pins, and the netlist comparison needs GPIO to pad | Proposed |
| B-P2-05 | §6.1 O1 | Onboard LED GPIO still open | Close as not applicable to the custom PCB. GPIO 38 and 48 become spares, subject to B-P2-04 | There is no dev-board LED on the module | Proposed |
| B-P2-06 | §2.1 | "Camera retained" | Rev A: mechanical only, no electrical connection (D-P2-05) | The video chip is probably on the OEM board | Proposed, pending P2-1 inspection |
| B-P2-07 | §5 Phase 2 | "A compact, light 2- or 4-layer shield board" | "A compact, light single flight-controller board" (Block 1) | D-P2-01 | Proposed |
| B-P2-08 | §5 Phase 2 step 3 | "Motor supply traces 40 to 50 mil minimum" | Width calculated from the measured stall current and the allowed temperature rise (IPC-2221 external: $I = 0.048\,\Delta T^{0.44}A^{0.725}$, A in mil², *verify*), with pours where the board is too small | The fixed width ignores the real current | Proposed |
| B-P2-09 | §3.1 | No USB power path | Add the path chosen in P2-2 (Block 4) | D-P2-08 | Pending P2-2 |
| B-P2-10 | §6.1, §6.2 | O1 to O4, R1 to R8 | Add O5 to O8 and R9 to R11 (Block 6) | Phase 2 findings | Proposed |
