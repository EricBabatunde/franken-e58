# Franken-E58 PCB decision log

One row per design choice. Date in ISO form. State: Decided, Default (open), Proposed, Open, Rejected. Decisions carried in from the Phase 2 blocks (`docs/Phase2_blueprint_blocks.md`, Block 1) are listed first; new decisions are appended below them as they are made.

| Date | ID | Decision | Reason | State |
|---|---|---|---|---|
| 2026-11-11 | D-P2-01 | Single PCB with the ESP32-S3 module soldered on. No dev board, no shield | A 44-pin dev board is too tall and heavy for a micro-quad cavity; mass and height are the main constraints | Decided |
| 2026-11-11 | D-P2-02 | Module: ESP32-S3-WROOM-1-N16R8 by default | Matches the module family validated in Phase 1 (flash and PSRAM modes, `sdkconfig`); any other module needs the §4.9 options re-verified | Default, open (close in P2-2, O3) |
| 2026-11-11 | D-P2-03 | USB-C receptacle with D+ and D− on GPIO 20 and 19 (native USB Serial/JTAG) as the programming, logging and debug port; BOOT and EN buttons and UART0 pads as fallback | Same native port as Phase 1; no USB-UART bridge needed | Decided in principle (height and mass checked in P2-1, power path in P2-2) |
| 2026-11-11 | D-P2-04 | IMU at the geometric centre and centre of gravity, away from the antenna, LDO, module and motor currents, over solid ground; whole board soft-mounted | Least noise (R1) | Decided (slot or cut-out option reserved) |
| 2026-11-11 | D-P2-05 | Camera: mechanical only in Rev A (mount and cable clearance kept, no electrical connection) | The video and Wi-Fi chip is probably on the OEM board, and a DVP camera needs about 12 GPIOs | Pending the P2-1 group F inspection |
| 2026-11-11 | D-P2-06 | ToF: breakout on a connector in Rev A, chip on board later | The VL53L1X is a small optical LGA that needs a cover window | Proposed (confirm in P2-2) |
| 2026-11-11 | D-P2-07 | Assembly method: hand-solder or fab assembly | Decides package choices (LGA, QFN, passive size) | Open (default fab assembly of SMD parts; close in P2-2) |
| 2026-11-11 | D-P2-08 | Power path between USB 5 V and the 1S battery | Phase 3 needs USB logging while the motors run from the battery; VBUS must never back-feed the battery or reach the motor rail | Open (close in P2-2; default option B, source-select jumper) |
