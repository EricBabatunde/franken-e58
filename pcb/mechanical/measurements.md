# Mechanical and electrical measurements (P2-1)

**Status: not started.** Filled in during the P2-1 measurement interview (`docs/P2-1_package.md` §2). Signed-off values are copied into `constraints.csv`.

## Datum and orientation

All coordinates are from **one datum**: the **lower-left corner of the OEM board seen from the top**, with **x towards the aircraft's right-hand side** and **y towards the nose**. Z is up from the board's top surface. Keep this convention in every file in `pcb/mechanical/`.

## Tags

Every number carries one tag. A missing number is asked for, never estimated.

| Tag | Meaning |
|---|---|
| `[measured: <tool>, <date>]` | Read on the real part with the named tool (calipers, steel ruler plus photo, multimeter) |
| `[datasheet p.N]` | Taken from a datasheet page listed in `pcb/datasheets/MANIFEST.md` |
| `[assumed]` | Not measured; listed in the stage report and replaced before the gate |

Photos go in `photos/` with a scale in frame, named like `oem_top.jpg`, `oem_bottom.jpg`, `cavity_top.jpg`, `cavity_side.jpg`, `conn_<name>.jpg`, `camera_conn.jpg`.

## Tools available

| Question | Answer |
|---|---|
| Digital calipers? | |
| Fallback (steel ruler plus top-down photo with scale, about ±0.5 mm)? | |
| Multimeter (model, 10 A range)? | |

## Group A: OEM board

| Item | Value | Tag | Note |
|---|---|---|---|
| Length (x) | | | |
| Width (y) | | | |
| Thickness | | | |
| Corner radii | | | |
| Notches or cut-outs (position, size) | | | |
| Copper-free zones | | | |
| Mass | | | |
| Photos top and bottom (file names) | | | |

## Group B: Mounting

| Item | Value | Tag | Note |
|---|---|---|---|
| Number of screw posts or holes | | | |
| Hole 1 position (x, y) and diameter | | | |
| Hole 2 position (x, y) and diameter | | | |
| Hole 3 position (x, y) and diameter | | | |
| Hole 4 position (x, y) and diameter | | | |
| Post outer diameter | | | |
| Post height | | | |
| Screw type and length | | | |
| Posts on the frame or on the board? | | | |

## Group C: Frame cavity (the frame decides the fit, not the OEM board)

| Item | Value | Tag | Note |
|---|---|---|---|
| Internal length at the seat (x) | | | |
| Internal width at the seat (y) | | | |
| Wall positions relative to the datum | | | |
| Height available under the canopy (above the board top) | | | |
| Height available below the board (if any) | | | |
| Clearance to the battery bay | | | |
| Clearance to the motor arms | | | |
| Ribs, posts or bosses that intrude (position, size, height) | | | |
| Photos (file names) | | | |

## Group D: Battery

| Item | Value | Tag | Note |
|---|---|---|---|
| Connector type | | | |
| Pitch | | | |
| Polarity (which pin is +) | | | |
| Wire gauge | | | |
| Connector position (x, y) and exit direction | | | |
| Cell dimensions | | | |
| Cell mass | | | |
| Cell marking, capacity, rating | | | |

## Group E: Motors

| Item | Value | Tag | Note |
|---|---|---|---|
| Connection type (plug or solder pads) | | | |
| Connector type and pitch | | | |
| Motor 1 (front right) position, wire length, polarity convention | | | |
| Motor 2 (rear right) position, wire length, polarity convention | | | |
| Motor 3 (rear left) position, wire length, polarity convention | | | |
| Motor 4 (front left) position, wire length, polarity convention | | | |
| Propeller direction per arm (CW or CCW, feeds σ, §4.5) | | | |
| Motor mass (each) | | | |

## Group F: Camera

| Item | Value | Tag | Note |
|---|---|---|---|
| Where the camera connects (OEM board position x, y) | | | |
| Connector type (ZIF/FFC or other), pin count, pitch | | | |
| Module size and mass | | | |
| Cable length and route | | | |
| Mount and clearance needed | | | |
| Video chip on the camera module, or sensor only? | | | |
| Signals reaching the OEM board (continuity check, power off, pin by pin) | | | |
| Photo (file name) | | | |

## Group G: Motor electrical (stall current, B-P2-02)

Procedure and safety rules: `docs/P2-1_package.md` §4 step 6. Props off, one motor at a time, 1 to 2 s per reading, 30 s cool-down, three readings per motor. Readings and derived limits go to `pcb/specs/motor_electrical.md`; this table holds the raw values.

| Item | Value | Tag | Note |
|---|---|---|---|
| Supply voltage used | | | |
| Fuse rating in line | | | |
| Motor 1 stall readings (A, three) | | | |
| Motor 2 stall readings (A, three) | | | |
| Motor 3 stall readings (A, three) | | | |
| Motor 4 stall readings (A, three) | | | |
| Running current, if measured | | | |
| Wire resistance, if easy to read | | | |

## Group H: Other

| Item | Value | Tag | Note |
|---|---|---|---|
| LEDs on the OEM board (positions, visible through the shell?) | | | |
| Switches (type, position) | | | |
| Antenna wire (position, route) | | | |
| Any other OEM item the new board must keep a place for | | | |
| USB-C envelope check: receptacle height (3.2 mm class) against cavity height (*verify against the connector datasheet in P2-3*) | | | |
| USB-C envelope check: receptacle length (about 9 mm class) and reachable edge without removing the canopy | | | |

## Group I: Mass budget

| Item | Mass (g) | Tag | Note |
|---|---|---|---|
| OEM board | | | |
| Frame | | | |
| Four motors and props | | | |
| Battery | | | |
| Camera | | | |
| Wiring | | | |
| Total flying mass (assembled aircraft, if available) | | | |

## Sign-off

| Item | Value |
|---|---|
| `constraints.csv` signed off by | |
| Date | |
