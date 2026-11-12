# Mechanical and electrical measurements (P2-1)

**Status: interview complete and constraints signed off 2026-11-12.** Filled in during the P2-1 measurement interview (`docs/P2-1_package.md` §2). Signed-off values are copied into `constraints.csv`.

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
| Digital calipers? | No (2026-11-11) |
| Fallback (steel ruler plus top-down photo with scale, about ±0.5 mm)? | Yes: ruler. Every length is tagged `[measured: ruler, <date>]` with ±0.5 mm unless a photo with a scale gives better |
| Multimeter (model, 10 A range)? | DT9205A. No 10 A range; it has a 20 A range (10 mA resolution). Stall readings use the 20 A range for 1 to 2 s only. The 20 A input is assumed unfused and time-limited (*verify against the DT9205A manual*). Deviation from the package's "10 A range" wording, listed in the stage report |

## Group A: OEM board

| Item | Value | Tag | Note |
|---|---|---|---|
| Length (the longer side; runs nose to tail, so along **y** in the datum) | 46.5 mm | `[measured: ruler, 2026-11-12]` | Ruler precision 1 mm; reads between 46 and 47, so ±0.5 mm. Confirm from a scaled photo |
| Width (across the fuselage, along **x**) | 23 mm | `[measured: ruler, 2026-11-12]` | ±0.5 mm |
| Thickness | 1.0 mm | `[measured: ruler, 2026-11-12]` | Ruler at 1 mm precision; 1.0 mm is the nearest standard FR-4 thickness. ±0.2 mm |
| Corner radii | 0 (square, sharp corners) | `[measured: visual, 2026-11-12]` | |
| Notches or cut-outs (position, size) | 2 large mounting notches on the lower half (towards y = 0); 4 separate screw holes on the upper half | `[measured: visual, 2026-11-12]` | Positions and sizes: see group B. Both are retained on the new board so it fits the frame |
| Copper-free zones | None obvious | `[measured: visual, 2026-11-12]` | |
| Mass | not weighed | `[assumed]` | No scale available. The mass budget (group I) carries a safety factor on every assumed mass until weighed |
| Photos top and bottom (file names) | `photos/oem_top.jpg` (DSC00194, ruler along the length), `photos/oem_top_width_scale.jpg` (DSC00200, ruler along the width), `photos/oem_bottom.jpg` (DSC00187, no ruler), `photos/cavity_top.jpg` (DSC00181), `photos/frame_overview.jpg` (DSC00182), `photos/battery_label.jpg` (DSC00173), `photos/cavity_posts.jpg` (DSC00181 crop, board removed), `photos/board_in_frame.jpg` (DSC00176 crop). The rest of the 2026-11-12 set stays in the git-ignored `pcb/photos/` | | Ruler in frame |

## Group B: Mounting

| Item | Value | Tag | Note |
|---|---|---|---|
| Number of screw posts or holes | 4 round holes near the nose end, plus 2 semicircular cut-outs near the tail end | `[measured: visual, 2026-11-12]` | Pattern is symmetric about the board centreline. Both features are retained on the new board |
| Hole 1 (nose end, aircraft-left side, x = 0 edge) position and diameter | x = 1.35, y = 43.7, dia 1.5 | y `[measured: photo scaled on the 46.5 mm length, 2026-11-12]` ±0.5 mm; x and diameter `[measured: ruler gap and hole + photo, 2026-11-12]` ±0.3 mm | Hole centre 2.8 mm from the nose-end edge (photo). Corrected at sign-off: inner gap between the two holes across the width 18.8 mm (ruler), hole 1.5 mm (ruler, screw passes freely), so the cross pitch is 20.3 and x = (23 − 20.3)/2 = 1.35; rim to the edge 0.6 mm. Photo centres 1.15 to 1.3 mm agree. The earlier ruler values (pitch 21, centre 2 mm from the edge) were inconsistent with the 23 mm width and are superseded |
| Hole 2 (nose end, aircraft-right side) position and diameter | x = 21.65, y = 43.7, dia 1.5 | same | Cross pitch hole 1 to hole 2: **20.3 mm** (18.8 gap + 1.5 hole) |
| Hole 3 (aircraft-left side) position and diameter | x = 1.35, y = 33.0, dia 1.5 | same | Pitch hole 1 to hole 3 along the edge: **10.7 mm `[measured: ruler]`** |
| Hole 4 (aircraft-right side) position and diameter | x = 21.65, y = 33.0, dia 1.5 | same | |
| Cut-out A (aircraft-left edge, x = 0) | circular cut-out, diameter 5.8 ±0.5 (photo), centre at y = 16 ±0.5 and about 2.5 mm inside the edge; mouth at the edge 4.3 mm | y and mouth `[measured: ruler, 2026-11-12]`; diameter `[measured: photo]` | Ruler gives 16 mm from the tail-end edge; the photo gave 14.5, so the photo value is rejected (parallax: the bottom-side battery header tilts the board on the paper). Centre offset derived: sqrt(2.9² − 2.15²) ≈ 2 to 2.5 mm. Check at the 1:1 printout |
| Cut-out B (aircraft-right edge, x = 23) | same geometry mirrored, y = 16 ±0.5 | same | Same y as cut-out A within 0.5 mm (photo) |
| Board width check from photos | 23.5 to 24.1 mm across four photos, **rejected** | `[measured: photo, 2026-11-12]` | Re-measured by ruler: **23 mm**. The board does not sit flat on the paper (3-pin battery header on the bottom side), so the photos carry a tilt and parallax error of about 0.5 to 1.5 mm. Rule from here: photo values are used only where no ruler value exists, with ±1 mm |
| Post outer diameter (screw-hole posts) | 3 mm | `[measured: ruler, 2026-11-12]` | Four posts, part of the frame; the screws go down into them from above |
| Post height | not measured exactly | `[assumed]` | The four screw posts rise from the **lower** frame floor; the two cut-out posts rise from a **higher** frame floor. The camera sits in the space between the two floors, under the board. Heights to be read in group C |
| Screw type and length | thread not read; head about 2.3 mm, length a little over 4 mm; passes freely through the 1.5 mm hole | head and length `[measured: ruler, 2026-11-12]`; thread `[assumed: M1.4 class self-tapping]` | Thread diameter below 2 mm. Verify by fitting a known M1.4 and M1.6 screw if available |
| Posts on the frame or on the board? | On the frame | `[measured: visual, 2026-11-12]` | |
| Cut-out pegs | two round black pegs, about 4 mm diameter with a cross-shaped top, centres 18 mm apart at y = 16, rising from the slotted (higher) floor and passing up through the cut-outs | `[measured: photo board_in_frame.jpg scaled on the board length, 2026-11-12]` ±1 mm | Clearance peg to cut-out about 1 mm all round. Peg height above the board top: ask in group C |
| Frame model (from `cavity_posts.jpg`, `board_in_frame.jpg`) | Nose half of the board (y about 23 to 46.5) spans the **camera compartment**: a deep well whose floor carries the four 3 mm screw posts; the camera board with its ZIF ribbon connector sits on that floor between the posts, under the OEM board. Tail half (y about 0 to 23) lies over the **slotted battery-bay roof**, a higher floor with vent slots and two rectangular openings near its nose edge; the two pegs stand at its nose corners. The battery slides in under that roof from the tail. Two Phillips frame screws sit just outside the board's long edges near y = 8 (not board fixings). A copper-wound cylindrical part of about 6 mm sits in the nose compartment ahead of the board (function unknown; ask) | `[measured: visual, 2026-11-12]` | Heights of the two floors, the posts and the canopy are group C |
| Orientation (fixed) | Component side faces up in the frame. The large white 2-pin connector at the nose end is on the aircraft's **right** (x = 23 edge). Nose end = button end; tail end = antenna end | `[measured: visual, 2026-11-12]` | In `oem_top.jpg` (antenna left, nose right) the top edge of the image is the aircraft's left (x = 0) |

## Group C: Frame cavity (the frame decides the fit, not the OEM board)

| Item | Value | Tag | Note |
|---|---|---|---|
| Internal width wall to wall at board level (along x) | about 26 mm | `[measured: ruler, 2026-11-12]` | Board 23 mm, so about 1.5 mm side clearance each side |
| Cavity length beyond the board | nose: board nose edge to the camera-compartment front wall about 10 mm (y = 46.5 to 56.5). Tail: board tail edge to the end of the slotted floor about 21 mm (y = 0 to −21), with frame screw posts at 15 mm behind the tail edge (y = −15) | `[measured: ruler, 2026-11-12]` | Usable extra length at the tail is limited by those posts |
| Wall positions relative to the datum | x = −1.5 and x = 24.5 (from the 26 mm width, assuming the board is centred) | derived from the ruler width | Confirm centring at the 1:1 printout |
| Screw posts (four, camera compartment) | 18 mm from the camera-compartment floor to the post top on which the board sits | `[measured: ruler, 2026-11-12]` | So up to about 17 mm of depth is available **below** the board's nose half, less the camera board and ribbons that live there |
| Cut-out pegs | 7 mm tall from the slotted floor; four fins that widen at 3.5 mm to form the shoulder the board rests on. The board's underside is therefore about 3.5 mm above the slotted floor and the board does not touch it. Pegs stand about 2 mm above the board top (7 − 3.5 − 1.0 = 2.5 derived) | `[measured: ruler, 2026-11-12]` | Keep a no-component ring around each cut-out for the fins. Bottom-side parts on the tail half must be under 3 mm tall |
| Slotted floor height | about 14.5 mm above the camera-compartment floor (18 − 3.5) | derived | |
| Height available under the canopy (above the board top) | over the button at the nose end about 14 mm, where a 14 mm plunger moulded into the canopy underside presses the 8 mm-tall button; over the middle about 22 mm; over the antenna end about 26 mm | `[measured: ruler, 2026-11-12]` | The plunger position fixes where a power button can go if the OEM canopy is kept. Heights read without the top cover compressed; confirm with the cover fitted |
| Clearance to the battery bay | the battery runs under the slotted floor (tail half); the board's 3-pin battery header sits just nose-ward of the slotted floor's nose edge, reaching down into the camera compartment | `[measured: visual, 2026-11-12]` | Header position recorded in group D |
| Clearance to the motor arms | not measured; the arms pivot outside the cavity walls | `[assumed: no intrusion at board level]` | |
| Ribs, posts or bosses that intrude | the two rectangular openings in the slotted floor carry nothing; wires cross **above** the board; the camera FPCs and the small camera adapter PCB live below the nose half | `[measured: visual, 2026-11-12]` | The "copper cylinder" seen in the photos is the rolled camera FPC, not a coil |
| Photos (file names) | `photos/cavity_top.jpg`, `photos/cavity_posts.jpg`, `photos/board_in_frame.jpg`, `photos/cavity_side.jpg` (DSC00205; DSC00204 to 208 are oblique side views with the board fitted, used for reference only because the ruler's zero is not at board level) | | |

## Group D: Battery

| Item | Value | Tag | Note |
|---|---|---|---|
| Connector type | 3-pin male header on the **bottom** side of the board, pins pointing down; mates with three contacts on the top face of the cased battery pack, which slides in under the slotted floor from the tail | `[measured: visual, 2026-11-12]` | No wires: direct header to pack |
| Pitch | **2.0 mm** (outer contacts 4 mm centre to centre, three in a row along x) | `[measured: ruler on the pack contacts, 2026-11-12]` ±0.25 | The photo estimate of 2.5 is superseded |
| Polarity (which pin is +) | With the contacts facing the nose: **left contact positive, right contact negative** (aircraft left = x = 0 side), matching the B+ and B− silkscreen. Pack voltage 3.88 V. Middle contact: 0 V to negative and no resistance reading (open circuit) | `[measured: DT9205A, 2026-11-12]` | Middle contact treated as **not connected**; leave that pin unconnected on the new board. If the ohms reading was 0 Ω rather than open, it is a second negative instead: confirm |
| Wire gauge | not applicable (header) | | |
| Connector position (x, y) and exit direction | header centre about x = 14, y = 31 on the underside; the pack enters from the tail (−y). The pack latches by its shape, its pull tab sits flush with the body when latched, and its top face touches the underside of the slotted floor with no play | position `[measured: photo oem_bottom.jpg scaled on the board length, 2026-11-12]` ±1 mm; latching `[measured: visual]` | The header position is the one critical number here: confirm by ruler in group H before sign-off |
| Pack dimensions | cased pack **58 × 23 × 10 mm** (length from the contact end to the start of the trapezoidal pull tab, width, height). Pull tab length not measured | `[measured: ruler, 2026-11-12]` ±0.5 | The 802540 size code implies a 25 mm wide cell, which does not fit a 23 mm case, so the code or the label reading is doubtful; the case dimensions govern |
| Pack mass | not weighed, nothing stated on the pack or the box | `[assumed]` | Carried in the group I budget with a safety factor |
| Cell marking, capacity, rating | DWZ 802540, 3.7 V, 600 mAh, 2.22 Wh, date code 220521 (`photos/battery_label.jpg`) | `[measured: label, 2026-11-12]` | Discharge rating not on the label |

## Group E: Motors

| Item | Value | Tag | Note |
|---|---|---|---|
| Connection type (plug or solder pads) | plugs: four white 2-pin connectors on the top side, two on each long edge, horizontal entry facing outward | `[measured: visual, 2026-11-12]` | Bottom silkscreen marks each pair M+ and M− |
| Connector positions | on the aircraft-left edge (x = 0): y about 20.5 and 28.5; on the aircraft-right edge (x = 23): y about 20.5 and 28.5 | `[measured: photo oem_top.jpg scaled on the board length, 2026-11-12]` ±1 | Housings overhang the board edge slightly |
| Connector type and pitch | 2-pin, about 1.25 mm pitch (PicoBlade or GH class; exact family identified in P2-3 from the plug) | `[measured: ruler, 2026-11-12]` | Each connector sits at the position of the motor it serves: left-edge connectors for the left motors, right-edge for the right, nose-ward connector for the front motor on each side |
| Motor class | 716 coreless: 7 mm diameter, 16 mm long | `[measured: ruler, 2026-11-12]` | |
| Motor 1 (front right) | connector x = 23, y = 28.5; leads red and white; wire about 120 mm from motor to plug, tight, no slack | `[measured: ruler and visual, 2026-11-12]` | Prop marked **B**, turns **anticlockwise** seen from above |
| Motor 2 (rear right) | connector x = 23, y = 20.5; leads red and blue; about 120 mm, tight | same | Prop marked **A**, turns **clockwise** |
| Motor 3 (rear left) | connector x = 0, y = 20.5; leads red and white; about 120 mm, tight | same | Prop marked **B**, turns **anticlockwise** |
| Motor 4 (front left) | connector x = 0, y = 28.5; leads red and blue; about 120 mm, tight | same | Prop marked **A**, turns **clockwise** |
| Propeller direction per arm (feeds σ, §4.5) | FL = CW, FR = CCW, RL = CCW, RR = CW (diagonal pairs share a direction). Lead colour red/blue = CW motor, red/white = CCW motor | `[measured: visual, 2026-11-12]` | New-board connector positions must stay where the OEM ones are, or the 120 mm leads will not reach |
| Motor mass (each) | | `[assumed]` | No scale |

## Group F: Camera

| Item | Value | Tag | Note |
|---|---|---|---|
| Where the camera connects (OEM board position x, y) | provisional from the photos: a 3-pin connector at the nose end near the button (red, white, yellow lead going into the camera compartment). The bottom silkscreen near it reads V+, RX, TX, so this is probably a serial link plus power to the camera adapter PCB. A second, larger 2-pin connector at the nose end on the right edge carries a red/white pair towards the nose (LEDs?), with L− marked nearby | `[measured: photo, 2026-11-12]` | To be confirmed by continuity below |
| Connector type, pin count, pitch | To the main board: one 3-pin plug (red, black, yellow in pin order) into the 3-pin connector at the nose end next to the button. The cameras connect to the adapter PCB by two 20-pin FPCs (one per camera); no FPC reaches the main board | `[measured: visual, 2026-11-12]` | 3-pin connector family and pitch identified in P2-3 from the plug |
| Module size and mass | Two identical camera modules, nose and underside, glued to the frame; size and lens position not measurable without removing them | `[assumed: left in place]` | Mass assumed |
| Cable length and route | Both FPCs run inside the camera compartment to the adapter PCB; the 3-wire lead runs from the adapter PCB up to the main board's nose end, above the board | `[measured: visual, 2026-11-12]` | |
| Mount and clearance needed | Adapter PCB and the rolled FPCs occupy the camera compartment under the board's nose half (below the 18 mm post height). Nothing of the camera system sits above the board except the 3-wire lead | `[measured: visual, 2026-11-12]` | Adapter PCB size and position: see photos DSC00209 to 215 |
| Video chip on the camera module, or sensor only? | The adapter PCB carries a **Beken BK7231UQN40** Wi-Fi and Bluetooth SoC; the video encoding is almost certainly on the adapter too (the cameras are bare 20-pin sensor modules). The flight board carries no video or Wi-Fi function | chip marking `[measured: read on the part, 2026-11-12]`; encoder location `[assumed]` | |
| Signals reaching the OEM board (continuity, power off, pin by pin) | red = **V+** (also continuous with the MCU's **VCC** pad); black = **GND**; yellow = **RX** of the flight board. **TX** is not connected to the lead. The adapter therefore gets power and ground from the flight board and sends a one-way serial stream (app commands) into the flight board's RX | `[measured: DT9205A continuity, 2026-11-12]` | Voltage on V+ with the battery in: **to measure** (battery voltage or regulated?). Baud rate and protocol unknown and not needed for Rev A |
| D-P2-05 | Decided 2026-11-12: strictly mechanical, no electrical connection, no footprint (see `pcb/decision_log.md`). Consequence for layout: nose half of the board has no bottom-side parts other than the battery header, so the camera adapter and ribbons keep their space | decision | The V+ voltage and the adapter PCB size are no longer needed |
| LEDs | A separate large 2-pin connector at the nose end, right edge, feeds two white LEDs at the nose (power-on indication). Its return is the **L−** pad, so the flight board switches the LED low side | `[measured: continuity and visual, 2026-11-12]` | Whether Rev A keeps a 2-pin LED output: decide in P2-2 |
| Photo (file name) | `photos/camera_adapter_top.jpg` (DSC00213), `photos/camera_adapter_bottom.jpg` (DSC00214), `photos/camera_adapter_in_place.jpg` (DSC00212, nose camera lens visible). DSC00209 to 211 are further OEM board views, DSC00215 shows the adapter lifted with the bottom-camera ribbon | | No ruler in frame: adapter PCB size still to be read by ruler |
| Notes so far (from group C) | Two cameras: one at the nose and one on the underside. Both connect by FPC to a small adapter PCB in the camera compartment (the bottom camera is mounted on that PCB), which connects to the main board through 3-pin connectors | `[measured: visual, 2026-11-12]` | Group F pins this down: how many 3-pin leads reach the main board and which signals they carry |

## Group G: Motor electrical (stall current, B-P2-02)

Procedure and safety rules: `docs/P2-1_package.md` §4 step 6. Props off, one motor at a time, 1 to 2 s per reading, 30 s cool-down, three readings per motor. Readings and derived limits go to `pcb/specs/motor_electrical.md`; this table holds the raw values.

| Item | Value | Tag | Note |
|---|---|---|---|
| Method used (2026-11-12) | Flight pack connected straight across one motor with the meter in series (OEM board not connected). Stall: shaft held, reading taken. Running: same circuit, motor spinning freely with its prop on | [measured: DT9205A 20 A range, 2026-11-12] | Deviations from §4 step 6: flight pack used instead of a protected 18650, **no inline fuse**, one reading per motor instead of three, props on for the running readings. The readings are direct DC values, so they are usable; the repeats are still owed (see below) |
| Supply voltage used | Flight pack, 3.88 V open circuit (group D). Pack voltage during the stall not read | [measured: DT9205A, 2026-11-12] | The stall current therefore includes the pack's sag under load; a stiff 4.2 V supply would give a higher figure, see derived row |
| Fuse rating in line | None | [measured: 2026-11-12] | Package asks for 5 to 7.5 A. No fuse available, so the repeat readings are not taken in P2-1: one reading per motor stands, the direct re-measurement moves to Phase 3 on the new board (B-P2-02 already provides for it). To be listed as a deviation from acceptance criterion 4 at close-out |
| Motor 1 (front-right, M1) stall reading (A) | 2.92 | [measured: DT9205A 20 A, 2026-11-12] | Single reading; repeats deferred to Phase 3 (no fuse available) |
| Motor 2 (rear-right, M2) stall reading (A) | 2.93 | [measured: DT9205A 20 A, 2026-11-12] | Single reading; repeats deferred to Phase 3 (no fuse available) |
| Motor 3 (rear-left, M3) stall reading (A) | 2.93 | [measured: DT9205A 20 A, 2026-11-12] | Single reading; repeats deferred to Phase 3 (no fuse available) |
| Motor 4 (front-left, M4) stall reading (A) | 2.95 | [measured: DT9205A 20 A, 2026-11-12] | Single reading; repeats deferred to Phase 3 (no fuse available) |
| Running current, prop on, free air, direct from pack (A) | FR 1.50, RR 1.45, RL 1.50, FL 1.57 | [measured: DT9205A 20 A, 2026-11-12] | Full pack voltage, no PWM. This is the hover-class figure per motor; no-prop running current not measured |
| OEM board, "motors spinning, not flying" state | about 2.9 V read with the OEM board driving the motors; a 0.6 V figure was also reported for the same state | [measured: DT9205A, 2026-11-12] | Informational only, not used for sizing. Which point each voltage was read at is unresolved |
| Total circuit resistance at stall, derived | about 1.3 Ohm (3.88 V / 2.95 A) | [derived] | Motor winding plus leads, meter shunt and pack internal resistance. Stall at a stiff 4.20 V supply estimated at 3.2 to 3.5 A [derived]; use the measured 2.95 A plus this margin for sizing in `pcb/specs/motor_electrical.md` |
| Wire resistance, if easy to read | | | |

## Group H: Other

| Item | Value | Tag | Note |
|---|---|---|---|
| LEDs on the OEM board (positions, visible through the shell?) | One status LED at x = 11.5, y = 17 (board centre line, tail half), visible through the canopy | [measured: ruler, 2026-11-12] | Canopy transparency (whole shell or window) not recorded; the new board's status LED should sit near this spot |
| Switches (type, position) | One only: the power button, a 6 x 6 mm tactile switch, 8 mm tall, centre at x = 11, y = 38; long press switches the aircraft on. Pressed through the 14 mm canopy plunger (group C) | [measured: ruler, 2026-11-12] | The plunger fixes the power-button position on the new board if the OEM canopy is kept |
| Antenna wire (position, route) | About 30 mm long, leaves the tail edge at x = 1, y = 0 (the extreme corner of the board) | [measured: ruler, 2026-11-12] | Route inside the frame not recorded. Not needed for the new board (module has its own antenna); the corner stays free as an antenna keep-out candidate |
| Battery header position confirmed by ruler | Centre pin at x = 15, y = 32 (bottom side) | [measured: ruler, 2026-11-12] | Replaces the photo value (14, 31) in D1 |
| Any other OEM item the new board must keep a place for | None named | [measured: 2026-11-12] | |
| USB-C envelope check: receptacle height (3.2 mm class) against cavity height (*verify against the connector datasheet in P2-3*) | Pass. Canopy clearance above the board top is 22 mm mid-board and 26 mm at the tail (group C); under the board 3.5 mm to the slotted floor on the tail half | [measured: ruler, 2026-11-12] | A mid-mount or top-mount receptacle fits with large margin |
| USB-C envelope check: receptacle length (about 9 mm class) and reachable edge without removing the canopy | **Go, with a frame cut.** Receptacle on the aircraft-left long edge (x = 0) in the tail half, centre at y between 3 and 13, flush with the board edge. The frame side wall there is solid, reachable from outside, free of latches and wire channels, about 1 to 1.5 mm thick, and 1.5 mm from the board edge (cavity 26 mm, board 23 mm). A slot about 7 x 13 mm is cut in it (drill and file, not melted). Canopy is screwed on and has a flat free area, so a top-side vertical receptacle was the alternative; the user chose the side cut | [measured: ruler, 2026-11-12] | Zone: y = 3 to 13 keeps clear of the cut-out ring at y = 16 and the motor connectors at y = 20.5 and 28.5. Exact y and the receptacle part are chosen in P2-3 with the datasheet |

## Group I: Mass budget

| Item | Mass (g) | Tag | Note |
|---|---|---|---|
| OEM board with connectors | 6 | [assumed] | No scale and no published figure; class value, verify when a scale is available |
| Frame with canopy, arms and landing gear | 35 | [assumed] | Class value |
| Four 716 motors and props | 16 (4 x 4) | [assumed] | Class value; prop diameter 107 mm, diagonal shaft distance 200 mm, arms unfolded [measured: ruler, 2026-11-12] |
| Battery DWZ 802540, cased | 20 | [assumed] | Class value for an 802540 cell with case and contacts |
| Camera adapter with two cameras | 5 | [assumed] | Class value |
| Wiring and motor leads | 3 | [assumed] | Class value |
| Total flying mass (assembled aircraft, if available) | 85 assumed; 98 with a 15 % factor | [assumed] | Aircraft not assembled, no scale. The new board is budgeted at no more than the OEM board's 6 g; the budget is re-checked with a scale before P2-9 |
| Decisions taken here | D-P2-04: IMU at the geometric centre of the board (aircraft cannot be assembled now, so no balance measurement). D-P2-07 preference: the user hand-solders the components | [measured: 2026-11-12] | Recorded in `pcb/decision_log.md` |

## Sign-off

| Item | Value |
|---|---|
| `constraints.csv` signed off by | Eric |
| Date | 2026-11-12 |
| Basis | Groups A to I complete. Mounting holes B1 to B4 corrected at sign-off (centre 1.35 mm from the long edges, hole 1.5 mm, cross pitch 20.3 mm, from the 18.8 mm inner gap and the 1.5 mm hole; the earlier ruler values 21 and 2 were inconsistent with the 23 mm width). Open deviations: one stall reading per motor, no fuse, repeat deferred to Phase 3. |
