#!/usr/bin/env python3
"""Validate pcb/mechanical/outline.dxf against pcb/mechanical/constraints.csv.

P2-1 package, Prompt 3. Checks, each within TOL mm:
  1. DXF units are millimetres ($INSUNITS = 4).
  2. Only LINE, ARC and CIRCLE entities (no splines, text, blocks, dimensions).
  3. The LINE and ARC entities form exactly one closed outline.
  4. Outline starts at the datum and its bounding box matches A2 (width, x)
     and A1 (length, y) in the CSV.
  5. One CIRCLE per mounting hole B1 to B4, position and diameter as in the CSV,
     and no extra circles.
The cut-outs (B5, B6) are part of the closed outline; their arcs are printed
for information only.

Runs in pcb/tools/.venv (ezdxf). Shares no code with the SOLIDWORKS export.
Usage: pcb/tools/.venv/bin/python pcb/tools/check_outline.py [dxf] [csv]
"""
import csv
import math
import sys
from pathlib import Path

import ezdxf

TOL = 0.1          # mm, package acceptance criterion 5
JOIN_TOL = 1e-3    # mm, end points closer than this are treated as joined
HOLE_IDS = ("B1", "B2", "B3", "B4")

ROOT = Path.cwd()
DXF_PATH = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "pcb/mechanical/outline.dxf"
CSV_PATH = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "pcb/mechanical/constraints.csv"

results = []  # (name, ok, detail)


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))


def load_csv(path):
    with open(path, newline="") as f:
        return {row["id"].strip(): row for row in csv.DictReader(f)}


def num(row, key):
    v = row[key].strip()
    return float(v) if v else None


def arc_ends(e):
    return (e.start_point.x, e.start_point.y), (e.end_point.x, e.end_point.y)


def arc_extremes(e):
    """End points plus every axis extreme (0, 90, 180, 270 degrees) the arc passes."""
    pts = list(arc_ends(e))
    c, r = e.dxf.center, e.dxf.radius
    a0, a1 = e.dxf.start_angle % 360, e.dxf.end_angle % 360
    span = (a1 - a0) % 360 or 360
    for q in (0, 90, 180, 270):
        if (q - a0) % 360 <= span:
            pts.append((c.x + r * math.cos(math.radians(q)), c.y + r * math.sin(math.radians(q))))
    return pts


def main():
    cons = load_csv(CSV_PATH)
    doc = ezdxf.readfile(DXF_PATH)
    ents = list(doc.modelspace())

    insunits = doc.header.get("$INSUNITS", 0)
    check("units are millimetres", insunits == 4, f"$INSUNITS = {insunits} (4 = mm)")

    other = sorted({e.dxftype() for e in ents} - {"LINE", "ARC", "CIRCLE"})
    check("only LINE, ARC and CIRCLE entities", not other,
          f"other types: {other}" if other else f"{len(ents)} entities")

    segs = []
    for e in ents:
        if e.dxftype() == "LINE":
            segs.append(((e.dxf.start.x, e.dxf.start.y), (e.dxf.end.x, e.dxf.end.y), e))
        elif e.dxftype() == "ARC":
            s, t = arc_ends(e)
            segs.append((s, t, e))
    circles = [e for e in ents if e.dxftype() == "CIRCLE"]

    def same(p, q):
        return math.hypot(p[0] - q[0], p[1] - q[1]) < JOIN_TOL

    unused = list(range(len(segs)))
    loops = []
    while unused:
        i = unused.pop(0)
        start, cur = segs[i][0], segs[i][1]
        closed = same(start, cur)
        while not closed:
            nxt = None
            for j in unused:
                if same(segs[j][0], cur):
                    nxt, cur = j, segs[j][1]
                    break
                if same(segs[j][1], cur):
                    nxt, cur = j, segs[j][0]
                    break
            if nxt is None:
                break
            unused.remove(nxt)
            closed = same(start, cur)
        loops.append(closed)
    n_closed, n_open = loops.count(True), loops.count(False)
    check("exactly one closed outline", n_closed == 1 and n_open == 0,
          f"{n_closed} closed loop(s), {n_open} open chain(s), {len(segs)} segments")

    pts = []
    for s, t, e in segs:
        pts += arc_extremes(e) if e.dxftype() == "ARC" else [s, t]
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    w, l = max(xs) - min(xs), max(ys) - min(ys)
    w_csv, l_csv = num(cons["A2"], "x_mm"), num(cons["A1"], "y_mm")
    check("outline starts at the datum (0, 0)", abs(min(xs)) <= TOL and abs(min(ys)) <= TOL,
          f"min corner ({min(xs):.3f}, {min(ys):.3f})")
    check("width (x) matches A2", abs(w - w_csv) <= TOL, f"DXF {w:.3f}, CSV {w_csv}")
    check("length (y) matches A1", abs(l - l_csv) <= TOL, f"DXF {l:.3f}, CSV {l_csv}")

    remaining = list(circles)
    for hid in HOLE_IDS:
        row = cons[hid]
        hx, hy, hd = num(row, "x_mm"), num(row, "y_mm"), num(row, "size_mm")
        best = min(remaining, key=lambda c: math.hypot(c.dxf.center.x - hx, c.dxf.center.y - hy),
                   default=None)
        if best is None:
            check(f"{hid} hole present", False, "no circle left to match")
            continue
        remaining.remove(best)
        off = math.hypot(best.dxf.center.x - hx, best.dxf.center.y - hy)
        dia = 2 * best.dxf.radius
        check(f"{hid} position", off <= TOL,
              f"DXF ({best.dxf.center.x:.3f}, {best.dxf.center.y:.3f}), CSV ({hx}, {hy}), off {off:.3f}")
        check(f"{hid} diameter", abs(dia - hd) <= TOL, f"DXF {dia:.3f}, CSV {hd}")
    check("no extra circles", not remaining,
          ", ".join(f"({c.dxf.center.x:.2f}, {c.dxf.center.y:.2f}) d{2 * c.dxf.radius:.2f}"
                    for c in remaining))

    print(f"check_outline.py  dxf={DXF_PATH}  csv={CSV_PATH}  tol={TOL} mm")
    for name, good, detail in results:
        print(f"  [{'PASS' if good else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))
    arcs = [e for _, _, e in segs if e.dxftype() == "ARC"]
    if arcs:
        print("  info, cut-out arcs (not checked):")
        for e in arcs:
            c = e.dxf.center
            print(f"    centre ({c.x:.3f}, {c.y:.3f}) dia {2 * e.dxf.radius:.3f}")
    ok = all(r[1] for r in results)
    print("RESULT: PASS" if ok else "RESULT: FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
