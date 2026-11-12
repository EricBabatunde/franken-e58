# SOLIDWORKS brief: cavity envelope, board outline and OEM reference plate

**Stage:** Franken-E58 Phase 2, P2-1, package §4 step 5. **Written 2026-11-12, updated the same day with the M1 to M11 readings**, from the signed-off `constraints.csv` and `measurements.md`. This file is self-contained so it can be read on the Windows machine without the repo: every number needed is in here. The few that are still open are marked **OPEN** with a slot in §8.

**What you model:** the space the board lives in. **Not** the board design, **not** a redesign of the frame. Three models:

1. `cavity.step`: the cavity envelope, one solid body. Feeds the KiCad keep-outs and the 1:1 fit check.
2. `outline.dxf`: the board outline sketch with mounting holes and cut-outs. Feeds `check_outline.py` and the KiCad Edge.Cuts.
3. `oem_board_ref.step` (optional but cheap): a plate of the OEM board with its connectors as blocks, dropped into model 1 to catch measurement errors.

Keep the `.sldprt` and `.sldasm` files local (they are git-ignored). Only the STEP and DXF are tracked, in `pcb/mechanical/`.

---

## 1. Datum, axes and units

- **Units:** MMGS (millimetre, gram, second). Set it in Document Properties before the first sketch. Save As DXF and STEP inherit it.
- **Datum:** the lower-left corner of the OEM board seen from the top, at the board's **underside** (the seat).
- **Axes** (same as `measurements.md`):
  - **x** from the aircraft-left long edge towards aircraft right, 0 to 23 across the board;
  - **y** from the tail edge (antenna end) towards the nose (button end), 0 to 46.5 along the board;
  - **z** up from the board seat. The board occupies z = 0 to 1.0. Negative z is below the board (floors, posts), positive z is above (components, canopy).
- **SOLIDWORKS plane mapping, recommended:** sketch the board on the **Front plane** so that sketch horizontal = x, sketch vertical = y, and extrusions **out of the Front plane towards the viewer (+Z)** are "up". Then board x, y, z are exactly SOLIDWORKS X, Y, Z and the STEP needs no transform. (If you prefer the Top plane, remember SOLIDWORKS's vertical axis is Y, so heights become Y and the sketch vertical becomes −Z; the DXF is unaffected but write the mapping into the file name.)
- The part origin stays at the datum. Do not move it to the board centre.

## 2. Global variables

Enter these in Tools → Equations → Global Variables, then drive every sketch dimension from them, so a corrected measurement updates all three models. Names are suggestions; keep them if you can so the brief and the CSV stay readable side by side. Tags: R = ruler, P = photo, D = derived, A = assumed, all 2026-11-12.

| Variable | Value | Tag | CSV id | What it is |
|---|---|---|---|---|
| `L_board` | 46.5 | R | A1 | Board length along y |
| `W_board` | 23.0 | R | A2 | Board width along x |
| `T_board` | 1.0 | R | A3 | Board thickness |
| `R_corner` | 0 | visual | A4 | Corner radius (square) |
| `hole_d` | 1.5 | R | B1 | Mounting hole diameter (OEM; the new board's drill is chosen in P2-2) |
| `hole_x_left` | 1.35 | R+P | B1, B3 | Hole centre from the left edge |
| `hole_x_right` | 21.65 | D | B2, B4 | = `W_board` − `hole_x_left` (cross pitch 20.3) |
| `hole_y_nose` | 43.7 | P | B1, B2 | Nose pair, y |
| `hole_y_side` | 33.0 | P+R | B3, B4 | Side pair, y (10.7 below the nose pair, ruler) |
| `post_d` | 3.0 | R | B7 | Screw post outer diameter |
| `post_h` | 18.0 | R | C4 | Post height, camera floor to board seat |
| `cut_d` | 5.8 | P | B5 | Cut-out diameter |
| `cut_x_left` | 2.0 | D | B5 | Cut-out centre inside the left edge (from the 4.3 mouth and 5.8 dia) |
| `cut_x_right` | 21.0 | D | B6 | = `W_board` − `cut_x_left` |
| `cut_y` | 16.0 | R | B5, B6 | Cut-out centres, y |
| `peg_d` | 4.0 | P+R | B9 | Frame peg diameter, concentric with the cut-outs, 19.0 apart (ruler) |
| `peg_h` | 7.0 | R | C5 | Peg height above the slotted floor |
| `fin_d_base` | 7.0 | R | C16 | Fin span at the slotted floor |
| `fin_d_seat` | 4.0 | R | C16 | Fin span at board level (the fins taper) |
| `cavity_w` | 26.0 | R | C1 | Wall to wall at board level |
| `wall_gap` | 1.5 | D | C1 | = (`cavity_w` − `W_board`) / 2, assumed symmetric about x = 11.5 |
| `wall_t` | 1.25 | R | H5 | Side wall thickness (1 to 1.5) |
| `wall_top_z` | 4.5 | R | C14 | Side wall height above the board seat; the canopy sits on the walls |
| `front_wall_y` | 56.5 | R | C2 | Camera-compartment front wall |
| `floor_end_y` | −21.0 | R | C3 | Tail end of the slotted floor |
| `frame_post_y` | −15.0 | R | C3 | Frame posts behind the board (outside the cavity, reference only) |
| `floor_step_y` | **OPEN** (29 if the 13 mm was read from the pegs) | R | C11 | Nose edge of the slotted floor, where the camera well begins. See §8, M1 |
| `slot_floor_z` | −3.5 | R | C6 | Slotted floor top, below the board seat |
| `slot_floor_t` | 1.0 | R | C17 | Slotted floor thickness |
| `cam_floor_z` | −18.0 | D | C4 | Camera-compartment floor = −`post_h` |
| `cam_h` | 7.0 | D | C15 | Camera adapter assembly height above the camera floor (gap to the board underside 11) |
| `peg_top_z` | 3.5 | D | C5, C6 | = `slot_floor_z` + `peg_h`; stands 2.5 above the board top |
| `canopy_z_tail` | 26.0 | R | C10 | Canopy underside above the board **top** at y = 0 |
| `canopy_z_mid` | 22.0 | R | C9, C12 | at y = 23 and from there to the front wall, flat across the width (C13) |
| `plunger_len` | 14.0 | R | C8 | Plunger from the canopy underside down to the top of the 8 mm button (22 − 8 = 14) |
| `button_x`, `button_y` | 11.0, 38.0 | R | H1 | Power button centre, 6 × 6 body, 8 tall |
| `led_x`, `led_y` | 11.5, 17.0 | R | H2 | OEM status LED |
| `bat_hdr_x`, `bat_hdr_y` | 15.0, 32.0 | R | D1 | Battery header centre pin, bottom side, 3 pins along x at 2.0 pitch |
| `bat_L`, `bat_W`, `bat_H` | 58, 23, 10 | R | D2 | Battery pack, cased, slides in from the tail under the slotted floor |
| `mot_conn_y_rear`, `mot_conn_y_front` | 20.5, 28.5 | P | E1 to E4 | Motor connectors on both long edges, horizontal entry facing outward |
| `conn_h` | 5.0 | R | E5 | Edge connector height above the board top |
| `led_conn_x`, `led_conn_y` | 23, 42 | P | F2 | LED 2-pin connector, right edge |
| `cam_conn_y` | 44 | P | F1 | Camera 3-pin connector, x not read (not needed on the new board) |
| `usb_y_min`, `usb_y_max` | 3, 13 | R | H4 | USB-C receptacle zone on the left edge, flush |

Note the canopy heights are measured **above the board top**, so in z they are `T_board` + value: 27.0 at the tail and 23.0 from mid-board to the nose.

## 3. Model 1: cavity envelope (`cavity.step`)

One part, one solid body: the frame material that limits or touches the board. Build each feature with **Merge result** on so the end result is a single body. Skip threads, small fillets, draft and the outside of the frame.

Build order and geometry, all from the variables:

1. **Camera-compartment floor.** Slab under the nose half: x from −`wall_gap` to `W_board` + `wall_gap`, y from `floor_step_y` to `front_wall_y`, top face at z = `cam_floor_z`, 1 mm thick (`slot_floor_t`, cosmetic).
2. **Slotted floor** (battery-bay roof) under the tail half: same x span, y from `floor_end_y` to `floor_step_y`, top face at z = `slot_floor_z`, thickness `slot_floor_t`. The slots themselves are not modelled. The vertical step between the two floors at y = `floor_step_y` is a wall from z = `cam_floor_z` to `slot_floor_z`.
3. **Four screw posts:** cylinders of `post_d` from z = `cam_floor_z` to z = 0 at (`hole_x_left`, `hole_y_nose`), (`hole_x_right`, `hole_y_nose`), (`hole_x_left`, `hole_y_side`), (`hole_x_right`, `hole_y_side`). Their outer edge reaches x = −0.15 and 23.15, slightly outside the board edge; that is real, the board rests on the inner part of each post.
4. **Two pegs with fins:** at (`cut_x_left`, `cut_y`) and (`cut_x_right`, `cut_y`). Each is a cylinder of `peg_d` from z = `slot_floor_z` to z = `peg_top_z`, merged with a **cone** (revolve) from diameter `fin_d_base` at z = `slot_floor_z` to diameter `fin_d_seat` at z = 0. The cone is the envelope of the four fins; it is what bottom-side parts near the cut-outs must clear.
5. **Side walls:** at x = −`wall_gap` and x = `W_board` + `wall_gap`, thickness `wall_t` outward, from the floor up to z = `wall_top_z`, running y from `floor_end_y` to `front_wall_y`. Cut the **motor-lead slots** through both walls at y = `mot_conn_y_rear` and `mot_conn_y_front`, open at the wall top; width and depth are OPEN (M8 in §8), use 3 wide and 3 deep from the wall top as placeholders and label them `[assumed]`.
6. **Front wall** at y = `front_wall_y`, full width, same height. The tail end is **open** (the battery enters there, under the slotted floor).
7. **Canopy underside as a lid.** The canopy sits on the 4.5 mm walls and provides the rest of the height. Model a thin shell whose underside follows this profile in the y–z plane: z = 27.0 at y = 0, falling to z = 23.0 at y = 23, then **flat at 23.0** to the front wall. Flat across x. Extrude across the cavity width and merge. Add the **plunger** as a cylinder of diameter 6 [assumed] from the lid down `plunger_len` to z = 9.0 (board top 1.0 plus the 8 mm button) at (`button_x`, `button_y`). Between z = `wall_top_z` and the lid the model has open sides; that is correct, the canopy skirt is not modelled.
8. **Camera adapter assembly** on the camera floor: a block from z = `cam_floor_z` to z = `cam_floor_z` + `cam_h` = −11.0. Footprint: between the four posts, y from `floor_step_y` + 2 to `front_wall_y` − 2 [assumed]. Only its height matters: it sets the free depth under the nose half of the board at 11 mm.
9. **Battery bay** (reference only, does not touch the board): a 23 × 58 × 10 block under the slotted floor, top face at z = `slot_floor_z` − `slot_floor_t` = −4.5, centred on x = 11.5, running from y = `floor_end_y` towards the nose. Keep it as a **separate body** or leave it out, so the exported envelope is still one body.
10. **Small posts inside the cavity** (C19, OPEN, M9): they exist; positions and sizes are not yet measured. Add them as cylinders once measured. Until then the model is marked incomplete in this one respect.
11. **USB-C opening, as a sketch only, not a cut:** on the left wall, y from `usb_y_min` to `usb_y_max`. The receptacle sits at about z = 1.0 to 4.2 on the board, so the plug opening (about 7 tall) runs from the wall top down to about z = −1 and continues up into the canopy lip. It records where the frame and canopy will be cut in P2-9; neither is redesigned here.

**Export:** File → Save As → STEP AP214, "Solid/Surface geometry", confirm **one body** in the export preview, as `cavity.step`.

## 4. Model 2: board outline sketch (`outline.dxf`)

**Decision for Rev A: the outline is identical to the OEM board**, 46.5 × 23, square corners, same holes and cut-outs. Reasons: the long edges cannot move inward because the mounting holes sit 1.35 from the edge with only a 0.6 rim; shortening the ends gains nothing (10 free ahead of the nose, 21 behind the tail); and an identical outline makes the 1:1 printout a direct comparison with the OEM board. Do not offset inward.

Sketch, on the Front plane with the origin at the datum corner:

- Rectangle from (0, 0) to (`W_board`, `L_board`).
- Four circles, diameter `hole_d`, at the four hole positions in §2.
- Two cut-outs: circles of diameter `cut_d` centred at (`cut_x_left`, `cut_y`) and (`cut_x_right`, `cut_y`), **trimmed** so the edge line is cut away between the two intersections and the arc remains. The resulting mouth at the edge is 2·√(2.9² − 2.0²) = 4.2, matching the measured 4.3.
- Lines and arcs only, no splines. Delete all construction geometry. The result is **exactly one closed profile** plus four circles.

**Optional widening, your call, record it in the CSV if taken:** the cavity is 26 wide and the posts stand 0.15 proud of the OEM edge, so a 24 mm board (x from −0.5 to 23.5) is possible with a 1 mm wall gap and a 1.1 mm hole rim. Default is 23.0. If you widen, `W_board` = 24 and every x shifts by +0.5, including the datum, so tell Claude Code which you chose.

**Export:** the most reliable route is to extrude this sketch into the reference plate (model 3) and export its **top face**: right-click the face → Export to DXF/DWG, or File → Save As → DXF with "Faces / loops / edges" and the face selected. Units mm, version R2013 or later, no annotations, no sheet border, no axes. Check the preview shows only the outline, the two cut-out arcs and the four circles.

**What `check_outline.py` will test** (Prompt 3), each within 0.1 mm: units are millimetres; exactly one closed outline; bounding box 46.5 × 23.0 (or 46.5 × 24.0 if widened, with the CSV updated); four circles of 1.5 at the CSV positions. The cut-outs are part of the closed profile and are not checked separately in the first version.

**Note for P2-2, do not decide here:** the blueprint's soft mounting on silicone grommets (§3.3) needs holes of about 3 to 4 mm, which the 0.6 mm rim cannot give. Options to weigh in P2-2: widen the board to 24, soft-mount only the IMU, or mount rigidly on the posts and rely on the IMU filtering. The outline sketch stays as above until that decision.

## 5. Model 3: OEM board reference plate (`oem_board_ref.step`, optional)

A plate from the model 2 sketch, extruded `T_board` from z = 0 to 1.0, plus the connectors as rough blocks:

| Block | Position (centre) | Size | Height | Side |
|---|---|---|---|---|
| Power button | (11, 38) | 6 × 6 | 8 | top |
| Motor connectors, four | x = 0 and 23 edges, y = 20.5 and 28.5 | 4 along the edge × 3 inward [assumed] | 5 (E5) | top, edge-mounted |
| Battery header, 3 pins at 2.0 pitch along x | (15, 32) | about 7 × 3 | 5 [assumed] | **bottom** (it hangs into the camera well, 11 mm free) |
| LED 2-pin connector | (23, 42) at the right edge | 4 × 3 [assumed] | 5 | top |
| Camera 3-pin connector | y = 44, x not read | 5 × 3 [assumed] | 5 | top |
| Status LED | (11.5, 17) | 2 × 1.5 [assumed] | 1 | top |

Drop the plate into an assembly with model 1 at the same origin. The holes must land on the four posts, the cut-outs on the two pegs, the button under the plunger (plunger bottom touching the button top at z = 9), the battery header just nose-ward of the floor step, the motor connectors in line with the wall slots, and nothing may touch the camera adapter block. **If any of these does not line up, a measurement is wrong**; write down which and by how much, and Claude Code will reopen that row.

## 6. Files to produce

| File | Contents | Tracked |
|---|---|---|
| `pcb/mechanical/cavity.step` | Model 1, one body, STEP AP214, mm | yes |
| `pcb/mechanical/outline.dxf` | Model 2, mm, one closed profile plus four circles | yes |
| `pcb/mechanical/oem_board_ref.step` | Model 3 (optional) | yes |
| `pcb/mechanical/*.sldprt`, `*.sldasm` | sources | no (git-ignored) |
| 1:1 printout of the outline | package step 7: print, check the printer scale against a ruler, cut out, lay on the OEM board and in the frame | n/a |

## 7. Height summary for the KiCad keep-outs (derived from the above)

| Region of the board | Top-side limit above the board top | Bottom-side limit below the board underside |
|---|---|---|
| Tail half, y 0 to `floor_step_y` | canopy: 26 at y = 0 sloping to 22 at y = 23, then 22 | 3.5 to the slotted floor; keep bottom parts under 3 |
| Cut-out rings around (2, 16) and (21, 16) | pegs stand 2.5 proud: **no parts** within radius 3.5 of each centre, top side | fin cone 7 at the floor: **no parts** within radius 4 of each centre, bottom side |
| Nose half, `floor_step_y` to 46.5 | canopy 22 | 11 to the camera adapter |
| Plunger column at (11, 38) | the plunger comes down to z = 9: keep a 8 mm diameter clear above the button [assumed] | |
| Long edges at y = 20.5 and 28.5 | motor connectors, 5 tall, facing the wall slots | |
| Left edge, y 3 to 13 | USB-C receptacle, flush with the edge | |
| Walls | side walls end 4.5 above the seat; above that only the canopy | |

## 8. Open measurements

Everything else is filled in. Take these when convenient; the models can be started without them if the placeholders are labelled `[assumed]`.

| Id | What | Status |
|---|---|---|
| M1 | **Floor step reference point.** The 13 mm reading cannot be from the tail edge, because the pegs at y = 16 rise from the slotted floor. If it was read from the pegs, the step is at y = 29, which fits the battery header at y = 32 hanging just nose-ward of it. Confirm which point the 13 was measured from, or read the step directly from the tail edge of the board (expected about 29) | **OPEN** |
| M8 | Motor-lead wall slots: width along y and depth down from the wall top, at y = 20.5 and 28.5 on both walls | OPEN, placeholders 3 × 3 |
| M9 | Small posts inside the cavity: x, y, diameter and height of each, and whether any touches the board underside | OPEN, to be measured by the user with the model open |
| M2, M3, M4, M5, M6, M7, M10, M11 | Canopy 22 to the nose, flat across; walls 4.5; adapter gap 11; fins 7 to 4; floor 1; connectors 5; pegs 19 apart | done, in §2 |

## 9. Checks before you leave Windows

1. Document units MMGS; origin at the datum corner; axes as in §1.
2. All dimensions driven by global variables; change `W_board` to 24 and back to see the outline, holes and posts move together, then save at 23 (or at 24 if you chose to widen).
3. Model 1 is one body; posts' top faces at z = 0; pegs' tops at z = 3.5; wall tops at z = 4.5; lid at 23.0 over the nose half and 27.0 at the tail; plunger bottom at z = 9.0 over (11, 38); camera block top at z = −11.0.
4. Model 3 lands on the posts and pegs with no interference (Tools → Evaluate → Interference Detection in the assembly).
5. `outline.dxf` opened in any DXF viewer shows only the outline, two arcs and four circles, nothing else.
6. STEP re-imported into a fresh part shows one body at the same coordinates.
7. Print the outline 1:1 and check a 40 mm dimension on the print with the ruler before cutting it out.
