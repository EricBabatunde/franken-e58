# Franken-E58 PCB (Phase 2)

**Status: P2-1 in progress** (started 11 November 2026). P2-0 (toolchain) passed on 11 November 2026.

One compact, light, 2- or 4-layer flight-controller board carrying the ESP32-S3-WROOM-1 module, the MPU-6500, the VL53L1X connection, four low-side motor switches, the power stage and a USB-C programming port, fitting the E58 frame cavity (decision D-P2-01). The binding spec is `docs/Franken-E58_Full_System_Architecture_Blueprint.md` (revision 2.15). Phase 2 decisions and the pending blueprint diffs live in `docs/Phase2_blueprint_blocks.md`; each stage has a package in `docs/P2-n_package.md`.

## Stages and gates

| Stage | Work | Gate | State |
|---|---|---|---|
| P2-0 Toolchain | KiCad, MCP server, JLCPCB database, test board | `kicad` tools connected; test board passes ERC and DRC and exports | **Passed** 11 Nov 2026 |
| P2-1 Mechanical capture | OEM board, cavity, connectors, camera, stall current, mass, outline DXF, folder and rules | Constraints signed off; outline within 0.1 mm; 1:1 printout fits; stall current per motor | In progress |
| P2-2 Architecture decision | Close D-P2-02, 05, 06, 07, 08 and O3; mass budget; IMU rules; USB-C check | User approves the decision log; blueprint updated | |
| P2-3 Datasheet library | Every part plus the ESP32-S3 design guidelines and module datasheet | `MANIFEST.md` complete with URL, revision, date, SHA-256 | |
| P2-4 Specs | Net table, pin map, design rules | Every §3 part in the net table; pin map matches §3.4 as revised; stock checked | |
| P2-5 Schematic | Reference-design review per chip, then the schematic; land patterns checked | Every difference justified; power budget done | |
| P2-6 ERC and netlist check | ERC, netlist export, independent pin-by-pin script | ERC clean; 100 % of pins match; unused pins declared | |
| P2-7 Placement | IMU at centre, antenna keep-out, holes and connectors from P2-1 | User approves the placement render | |
| P2-8 Routing and DRC | Motor traces, I²C over ground, star ground; STEP fit check | DRC clean (MCP and `kicad-cli`); no cavity interference | |
| P2-9 Release | Gerbers, drill, BOM, pick-and-place, stock check, blueprint revision | User signs off; first-power-up plan written | |

## Folder

```
pcb/
├── README.md            this file
├── decision_log.md      every design choice, with date and reason
├── mechanical/          photos/, measurements.md, constraints.csv, outline.dxf, cavity.step
├── datasheets/          PDFs (git-ignored) and MANIFEST.md
├── specs/               requirements.md, motor_electrical.md, net_table.csv, pin_map.csv, design_rules.md
├── reviews/             one reference-design review per chip
├── kicad/franken-e58/   the KiCad project
├── kicad/lib/           project symbol and footprint libraries
├── tools/               outline check, netlist comparison, BOM and stock check (venv in tools/.venv, git-ignored)
└── outputs/             Gerbers, drill, BOM, pick-and-place, renders, STEP
```

## Opening the project

KiCad 10 (native package). Double-click `pcb/kicad/franken-e58/franken-e58.kicad_pro`, or run `kicad pcb/kicad/franken-e58/franken-e58.kicad_pro`. The project is created in P2-1 (Prompt 4) and holds only the outline and holes until P2-5.

## Working rules (short form; the full set is in `CLAUDE.md`)

- Datasheet first; every number tagged `[measured]`, `[datasheet p.N]` or `[assumed]`; no guessing.
- The KiCad GUI is never open on a file the `kicad` MCP server is editing. Board edits through the server go one at a time.
- Commit before every gate and before any bulk MCP edit.
- The firmware tree is not touched in Phase 2.
