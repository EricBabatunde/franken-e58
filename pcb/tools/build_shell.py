#!/usr/bin/env python3
"""Build the P2-1 KiCad shell: board outline, keep-out rule areas and height notes.

P2-1 package, Prompt 4 (approved plan 2026-11-20). The kicad MCP server has no
arc outline and no rule-area tool, so this script adds them with pcbnew. The
mounting holes are placed by the MCP server (add_mounting_hole), not here.

Every dimension is read from pcb/mechanical/constraints.csv by row id, except
the few constants below, which carry their CSV id and tag.

Coordinates: datum (lower-left corner of the board seen from the top, x to the
aircraft's right, y to the nose) sits at KiCad (100, 100) with the nose up:
    KiCad X = 100 + x,   KiCad Y = 100 - y.

Re-runnable: removes its own items first (Edge.Cuts shapes, rule areas named
"K...", and all Dwgs.User / Cmts.User items), never touches footprints.

Run (KiCad GUI closed, MCP server not holding the board):
    env -u PYTHONPATH pcb/tools/.venv/bin/python pcb/tools/build_shell.py
"""
import csv
import math
import sys
from pathlib import Path

import pcbnew

ROOT = Path(__file__).resolve().parents[2]
CSV_PATH = ROOT / "pcb/mechanical/constraints.csv"
PCB_PATH = ROOT / "pcb/kicad/franken-e58/franken-e58.kicad_pcb"

OX, OY = 100.0, 100.0          # datum position in KiCad (mm), set by MCP set_board_origin
CIRCLE_SEGS = 72               # rule areas are polygons; 5 degree steps (chord error < 0.004 mm at r 4)

# Constants not held in a numeric CSV column (tag and source on each line)
K1_R = 4.0        # cut-out ring radius: fin cone 7 across at the floor (C16) / 2 + 0.5 margin [assumed margin]
K2_R = 2.0        # around each mounting hole: screw head 2.3 (B8), post 3.0 (B7), plus margin [assumed margin]
K2_INNER_R = 1.5  # footprint keep-out inner radius, clears the hole footprint's own 2.0 pad box (corner r 1.414)
BATT_HDR = (12.0, 29.5, 18.5, 34.0)   # D1 note: housing x 12-18.5, y 29.5-34 [measured: SOLIDWORKS model 2026-11-20]
PLUNGER_D = 9.0   # C8 note: plunger root diameter 9 [measured: SOLIDWORKS model 2026-11-20]
USB_ZONE = (0.0, 3.0, 3.0, 13.0)      # H4: left edge, y 3 to 13; x depth 3 drawn for visibility only [assumed]
BOTTOM_TAIL_MAX = 2.5                 # C6: design limit 3.0 to the slotted floor minus 0.5 [derived]
CANOPY = "canopy underside above board top: 16.9 at y=0; 18.6 at y=23-38; 18.2 at y=47 (C9 C10 C12 model)"


def mm(v):
    return pcbnew.FromMM(v)


def K(x, y):
    """Datum coordinates (mm) to a KiCad VECTOR2I."""
    return pcbnew.VECTOR2I(mm(OX + x), mm(OY - y))


def load_csv():
    with open(CSV_PATH, newline="") as f:
        rows = {r["id"].strip(): r for r in csv.DictReader(f)}

    def f(rid, col):
        v = rows[rid][col].strip()
        if not v:
            sys.exit(f"constraints.csv {rid}.{col} is empty")
        return float(v)
    return f


def clear_own_items(board):
    for d in list(board.GetDrawings()):
        if d.GetLayer() in (pcbnew.Edge_Cuts, pcbnew.Dwgs_User, pcbnew.Cmts_User):
            board.Remove(d)
    for z in list(board.Zones()):
        if z.GetIsRuleArea() and z.GetZoneName().startswith("K"):
            board.Remove(z)


def add_line(board, layer, a, b, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(K(*a))
    s.SetEnd(K(*b))
    s.SetLayer(layer)
    s.SetWidth(mm(width))
    board.Add(s)


def add_arc(board, layer, start, mid, end, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_ARC)
    s.SetArcGeometry(K(*start), K(*mid), K(*end))
    s.SetLayer(layer)
    s.SetWidth(mm(width))
    board.Add(s)


def add_rect(board, layer, x0, y0, x1, y1, width=0.1):
    pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    for i in range(4):
        add_line(board, layer, pts[i], pts[(i + 1) % 4], width)


def add_circle(board, layer, cx, cy, d, width=0.1):
    s = pcbnew.PCB_SHAPE(board)
    s.SetShape(pcbnew.SHAPE_T_CIRCLE)
    s.SetCenter(K(cx, cy))
    s.SetEnd(K(cx + d / 2, cy))
    s.SetLayer(layer)
    s.SetWidth(mm(width))
    board.Add(s)


def add_text(board, layer, x, y, text, size=0.6):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(text)
    t.SetPosition(K(x, y))
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(mm(size), mm(size)))
    t.SetTextThickness(mm(size * 0.12))
    t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    board.Add(t)


def circle_pts(cx, cy, r):
    return [(cx + r * math.cos(2 * math.pi * i / CIRCLE_SEGS),
             cy + r * math.sin(2 * math.pi * i / CIRCLE_SEGS)) for i in range(CIRCLE_SEGS)]


def add_rule_area(board, name, outline, layers, holes=(), footprints=False, copper=False):
    """Keep-out rule area. footprints: no footprints. copper: no tracks, vias or copper pours."""
    z = pcbnew.ZONE(board)
    z.SetIsRuleArea(True)
    z.SetZoneName(name)
    ls = pcbnew.LSET()
    for l in layers:
        ls.AddLayer(l)
    z.SetLayerSet(ls)
    z.SetDoNotAllowFootprints(footprints)
    z.SetDoNotAllowTracks(copper)
    z.SetDoNotAllowVias(copper)
    z.SetDoNotAllowZoneFills(copper)
    z.SetDoNotAllowPads(False)
    poly = z.Outline()
    poly.NewOutline()
    for x, y in outline:
        v = K(x, y)
        poly.Append(v.x, v.y)
    for h in holes:
        hi = poly.NewHole()
        for x, y in h:
            v = K(x, y)
            poly.Append(v.x, v.y, 0, hi)
    board.Add(z)


def main():
    f = load_csv()
    W, L = f("A2", "x_mm"), f("A1", "y_mm")
    xl, yl, wl = f("B5", "x_mm"), f("B5", "y_mm"), f("B5", "size_mm")
    xr, yr, wr = f("B6", "x_mm"), f("B6", "y_mm"), f("B6", "size_mm")
    if abs(yl - yr) > 1e-6 or abs(wl - wr) > 1e-6:
        sys.exit("B5 and B6 differ in y or width; the outline code assumes a mirrored pair")
    yc, r = yl, wl / 2.0
    holes = [(f(h, "x_mm"), f(h, "y_mm")) for h in ("B1", "B2", "B3", "B4")]
    step_y = f("C11", "y_mm")
    plunger = (f("C8", "x_mm"), f("C8", "y_mm"))
    thickness = f("A3", "size_mm")

    board = pcbnew.LoadBoard(str(PCB_PATH))
    clear_own_items(board)
    board.GetDesignSettings().SetBoardThickness(mm(thickness))

    # Outline: one closed path, 10 lines and 2 arcs (U-slots open to each long edge)
    E = pcbnew.Edge_Cuts
    add_line(board, E, (0, 0), (W, 0))
    add_line(board, E, (W, 0), (W, yc - r))
    add_line(board, E, (W, yc - r), (xr, yc - r))
    add_arc(board, E, (xr, yc - r), (xr - r, yc), (xr, yc + r))
    add_line(board, E, (xr, yc + r), (W, yc + r))
    add_line(board, E, (W, yc + r), (W, L))
    add_line(board, E, (W, L), (0, L))
    add_line(board, E, (0, L), (0, yc + r))
    add_line(board, E, (0, yc + r), (xl, yc + r))
    add_arc(board, E, (xl, yc + r), (xl + r, yc), (xl, yc - r))
    add_line(board, E, (xl, yc - r), (0, yc - r))
    add_line(board, E, (0, yc - r), (0, 0))

    both = (pcbnew.F_Cu, pcbnew.B_Cu)
    # K1: cut-out rings, both sides, no footprints
    add_rule_area(board, "K1L cut-out ring (B5 B9 C16)", circle_pts(xl, yc, K1_R), both, footprints=True)
    add_rule_area(board, "K1R cut-out ring (B6 B10 C16)", circle_pts(xr, yc, K1_R), both, footprints=True)
    # K2: around each mounting hole: no copper (full disc), no footprints (annulus)
    for i, (hx, hy) in enumerate(holes, start=1):
        add_rule_area(board, f"K2c-B{i} screw copper keep-out (B7 B8)", circle_pts(hx, hy, K2_R), both, copper=True)
        add_rule_area(board, f"K2f-B{i} screw footprint keep-out (B7 B8)", circle_pts(hx, hy, K2_R), both,
                      holes=[circle_pts(hx, hy, K2_INNER_R)], footprints=True)
    # K3: nose half, bottom side, no footprints except the battery header (D-P2-05, C11, D1)
    bx0, by0, bx1, by1 = BATT_HDR
    add_rule_area(board, "K3 nose-half bottom: no parts (D-P2-05 C11 D1)",
                  [(0, step_y), (W, step_y), (W, L), (0, L)], (pcbnew.B_Cu,),
                  holes=[[(bx0, by0), (bx1, by0), (bx1, by1), (bx0, by1)]], footprints=True)

    # Notes on the user layers (not enforced by DRC)
    D, C = pcbnew.Dwgs_User, pcbnew.Cmts_User
    add_rect(board, D, 0, 0, W, step_y)
    add_text(board, C, 0.5, 1.0, f"BOTTOM: parts max {BOTTOM_TAIL_MAX} mm (C6)", 0.5)
    add_rect(board, D, *BATT_HDR)
    add_text(board, C, bx0, by0 - 0.9, "batt hdr (D1, bottom)", 0.5)
    add_circle(board, D, plunger[0], plunger[1], PLUNGER_D)
    add_text(board, C, plunger[0] - 2.5, plunger[1] + PLUNGER_D / 2 + 0.4, "button + plunger (C8 H1)", 0.5)
    add_rect(board, D, *USB_ZONE)
    add_text(board, C, 0.3, USB_ZONE[3] + 0.4, "USB-C zone (H4)", 0.5)
    legend = [
        "P2-1 shell: outline and holes from constraints.csv (outline.dxf, D-P2-09)",
        "Datum (0,0) = lower-left at KiCad (100,100); x right, y to the nose (up)",
        "K1 r4.0 cut-out rings: no footprints, both sides",
        "K2 r2.0 screw holes: no copper; no footprints r1.5-2.0",
        "K3 nose half y>=22.8 bottom: no footprints except battery header",
        f"Bottom tail half y<22.8: parts max {BOTTOM_TAIL_MAX} mm (C6)",
        "Top: " + CANOPY,
        "Hole rim to edge 0.2 mm: check fab hole-to-edge rule in P2-2",
    ]
    for i, line in enumerate(legend):
        add_text(board, C, W + 4, L - i * 1.2, line, 0.7)

    pcbnew.SaveBoard(str(PCB_PATH), board)
    n_edge = sum(1 for d in board.GetDrawings() if d.GetLayer() == E)
    n_rule = sum(1 for z in board.Zones() if z.GetIsRuleArea())
    print(f"saved {PCB_PATH.relative_to(ROOT)}: {n_edge} Edge.Cuts items, {n_rule} rule areas, "
          f"thickness {thickness} mm, board {W} x {L}, slots r {r} at ({xl}, {yc}) and ({xr}, {yc})")


if __name__ == "__main__":
    main()
