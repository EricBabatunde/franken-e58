# Design rules (draft, P2-2 carry-in C3)

**Status: draft.** Fab figures only. The routing rules (trace widths and clearances from the stall current) are added in P2-4.

**Fab:** JLCPCB (user, 2026-11-20).

**Source for every figure below:** https://jlcpcb.com/capabilities/pcb-capabilities. Fetched on 2026-11-20. The page shows no revision or "last updated" date; its footer reads © 2026. Each figure is tagged `[fab: JLCPCB capabilities page, 2026-11-20]`, shortened to **[fab]** in the tables.

Re-check this page before ordering (P2-9). It is a live web page, and it contradicts itself in places (noted below).

## 1. Board

| Rule | Value | Tag | Note |
|---|---|---|---|
| Minimum board size (FR4) | 3 × 3 mm | [fab] | Applies at thickness 0.6 mm or more. Our board is 23.5 × 47.0 |
| FR4 thickness options | 0.4, 0.6, 0.8, 1.0, 1.2, 1.6, 2.0 mm | [fab] | 1.0 mm (the OEM thickness, A3) and 0.8 mm are both offered. The page states no layer-count restriction for either |
| Thickness tolerance, 1.0 mm or more | ±10 % | [fab] | 1.0 mm board: 0.9 to 1.1 |
| Thickness tolerance, below 1.0 mm | ±0.1 mm | [fab] | 0.8 mm board: 0.7 to 0.9 |
| Outer copper, 2-layer | 1, 2, 2.5, 3.5, 4.5 oz | [fab] | |
| Outer copper, multilayer | 1, 2 oz | [fab] | |
| Inner copper | 0.5 (default), 1, 2 oz | [fab] | Specify 1 oz if inner planes carry current |

## 2. Outline and edges

| Rule | Value | Tag | Note |
|---|---|---|---|
| Routed outline tolerance | ±0.2 mm regular; ±0.1 mm high precision | [fab] | Applies to the U-slots and to the edge next to the mounting holes |
| Copper to routed edge | ≥ 0.2 mm | [fab] | Also applies to routed slots |
| Copper to V-cut edge | ≥ 0.4 mm | [fab] | Not used: single board, routed |
| Non-plated slot, minimum width | 1.0 mm | [fab] | U-slots are 4.2 mm |
| Non-plated slot tolerance | ±0.2 mm | [fab] | |
| Square inner corners on slots | Not supported | [fab] | The page gives no router radius. Square internal corners come out rounded. The outline's square corners are external, so this rule doesn't apply to them |
| **NPTH hole to board edge** | **Not stated on the page** | — | The only hole-to-edge figure on the page is for castellated holes (≥ 1 mm). Our case must be asked of JLCPCB or designed around (C3 below) |

## 3. Holes

| Rule | Value | Tag | Note |
|---|---|---|---|
| Minimum NPTH | 0.50 mm | [fab] | Draw NPTHs on the mechanical or keep-out layer (page wording) |
| Plated hole tolerance | +0.13 / −0.08 mm | [fab] | |
| NPTH round-hole tolerance | Not stated on the page | — | Slot tolerance ±0.2 mm is the only non-plated figure |
| Hole position tolerance | ±0.075 mm | [fab] | |
| Via hole / via pad, minimum | 0.15 / 0.25 mm (0.2 mm hole preferred) | [fab] | 0.1 / 0.2 mm only at 1 mm thickness or less, with ENIG or OSP |
| Via hole to hole | 0.2 mm | [fab] | |
| Pad hole to hole | 0.45 mm | [fab] | |
| PTH annular ring, 2-layer 1 oz | 0.25 mm recommended, 0.18 minimum | [fab] | |
| PTH annular ring, multilayer 1 oz | 0.20 mm recommended, 0.15 minimum | [fab] | |
| NPTH pad annular ring | ≥ 0.45 mm | [fab] | |

## 4. Traces

| Rule | Value | Tag | Note |
|---|---|---|---|
| Track / space, 1 oz, 1 and 2 layers | 0.10 / 0.10 mm | [fab] | |
| Track / space, 1 oz, multilayer | 0.09 / 0.09 mm | [fab] | |
| Track / space, 2 oz, 2-layer | 0.16 / 0.16 mm | [fab] | |
| Track / space, 2 oz, multilayer | 0.15 / 0.15 mm | [fab] | |
| Track width tolerance | ±20 % | [fab] | Matters for the motor traces: size at nominal × 0.8 (P2-4) |
| Minimum SMD pad | 0.25 × 0.25 mm | [fab] | |

## 5. Contradictions on the page (noted, not resolved)

- **Maximum board size:** the table and the note under it give different values. This doesn't matter for a 23.5 × 47 mm board.
- **Maximum thickness:** "0.4 to 4.5 mm" in one place, "0.4 to 3.2 mm" in the FAQ. This doesn't matter here.
- **Minimum plated slot:** the table says 0.35 mm for multilayer, the sentence below it says 0.5 mm. Not used yet.

## 6. Check of the P2-1 outline and holes (C3)

| Feature | Geometry (`constraints.csv`) | Fab rule | Result |
|---|---|---|---|
| Board size | 23.5 × 47.0 | ≥ 3 × 3 | OK |
| U-slots B5, B6 | 4.2 wide, r 2.1, open to the edge | Slot ≥ 1.0; tolerance ±0.2 (±0.1 high precision) | Manufacturable. With the pegs already a snug fit, a slot cut 0.1 to 0.2 mm narrow will bind. Scraping is the planned fix (P2-1) |
| Holes B1 to B4 (as of P2-1) | 2.0 dia, centre 1.2 from the long edge, so **0.2 mm rim** | Hole to edge: not stated. Outline ±0.2 (±0.1); hole position ±0.075 | **Not reliably manufacturable.** In the worst case the rim is 0.2 − 0.2 − 0.075 ≈ 0 with regular precision, or 0.025 with high precision. The hole can break out into the edge, and a 0.2 mm FR4 web can crack when the screw is tightened |
| Copper near holes | K2c keep-out r 2.0 | Copper to edge ≥ 0.2 | OK: no copper planned within 2.0 of a hole centre |

**Decision (user, 2026-11-20, P2-2 C3): option A.** Holes become 1.5 mm at the same centres, giving a 0.45 mm rim nominal. In the worst case the rim is 0.45 − 0.2 − 0.075 = 0.18 mm with the standard outline tolerance, or 0.28 mm with high precision. The screw passes a 1.5 mm hole freely (B8), and the 2.3 mm head clamps on a 0.4 mm ring. Options not taken: B (1.6 mm), C (open notches to the edge, kept as the fallback if a strip cracks) and D (keep 2.0).

Done 2026-11-20: the user updated `outline.dxf` to 1.5 mm holes and `check_outline.py` passes. Still to do: the KiCad holes change in P2-7. Ordering with the standard outline tolerance is acceptable; high precision is optional.
