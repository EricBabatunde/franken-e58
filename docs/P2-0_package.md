# Franken-E58 Phase 2, Stage P2-0: KiCad and AI toolchain

**Scope:** install KiCad natively on the Ubuntu laptop, install and register the mixelpixx KiCAD-MCP-Server with Claude Code, set up the JLCPCB parts database, and prove the chain end to end with a throw-away test board. No Franken-E58 design work is part of P2-0. The firmware repository is not changed, apart from the setup guide file that sits untracked in it.

Blueprint sections used: §4.1 (host tools), Phase 2 (objective and steps).

**Status: P2-0 passed** (11 November 2026). Blueprint revision 2.15 records Phase 2 (`docs/Phase2_blueprint_blocks.md`).

**Credentials:** none. The JLCPCB parts database is a public download. No API keys are stored.

---

## 1. Spec (what was set up)

| Item | Setting |
|---|---|
| KiCad | Native package from the KiCad PPA, **not** Flatpak, Snap or AppImage, because the MCP server needs `pcbnew` importable from system Python. Version: 10.0.8 (`kicad-cli version`) |
| KiCad API | Preferences → Plugins → **Enable KiCad API** ticked (live sync). Live sync was not exercised in P2-0 because KiCad was closed. All file-based tools work without it |
| MCP server | mixelpixx/KiCAD-MCP-Server, cloned to `~/mcp-servers/` (outside the repo). Commit used: `ac716d1` (kicad-mcp 2.7.0, the guide's tested version) |
| Runtime | Node and a Python virtual environment that can see `pcbnew`. Versions: Node v24.19.0, npm 11.17.0; venv on `/usr/bin/python3` 3.14.4 with `--system-site-packages`, `PYTHONPATH=/usr/lib/python3/dist-packages` |
| Registration | `claude mcp add-json --scope user kicad ...` (user scope, because the paths are absolute to the home folder). Takes effect in a new Claude Code session |
| Freerouting | **Skipped** by decision. Autorouting is not wanted for the motor and IMU areas |
| JLCPCB parts database | Installed. 511,178 parts, 359 MB. Used for stock and footprint checks in P2-4 |
| Progress file | `~/.kicad-mcp/setup-progress.md`, marked complete |
| Setup guide | The Manafish setup guide, followed with these overrides: native KiCad only, ask before any `sudo`, skip Freerouting, build the test board outside the repo |

## 2. Decisions

| ID | Decision | Reason |
|---|---|---|
| D-P2-0-01 | KiCad from the PPA, native install | A sealed Python (Flatpak, Snap, AppImage) hides `pcbnew` from the server |
| D-P2-0-02 | Server in `~/mcp-servers/`, registered at user scope | Keeps third-party code out of the repo. User scope because the paths are absolute |
| D-P2-0-03 | No Freerouting | Layout of the IMU and motor areas stays manual |
| D-P2-0-04 | Test board built in `~/Documents/KiCad/usb-c-led-test/`, outside the repo | A throw-away project must not pollute the flight repository |
| D-P2-0-05 | Wrap-up question in the guide (about the authors' drone kit) not asked | User instruction to skip wrap-up chat |

## 3. Acceptance criteria and results

| # | Criterion | Result |
|---|---|---|
| 1 | `kicad` tools answer in a new Claude Code session | PASS |
| 2 | JLCPCB database present and visible to the server | PASS (511,178 parts, 359 MB) |
| 3 | Test schematic passes ERC | PASS (0 violations) |
| 4 | Test board passes DRC in the MCP server and in `kicad-cli` | PASS (0 errors, 0 unconnected items in both) |
| 5 | Gerbers, drill, BOM and renders export | PASS (`usb-c-led-test-gerbers.zip`: nine fab layers plus Excellon drill and drill map; `bom.csv`; `board-top.png`; `board-3d.png`) |
| 6 | Franken-E58 repo untouched | PASS (apart from the untracked guide file) |

**Test board (reference only).** GCT USB4125 6-pin power-only USB-C socket, two 5.1 kΩ CC pull-downs, a 1 kΩ resistor and 0805 LED from VBUS to GND, power flags on VBUS and GND. Board 25 × 20 mm, two layers, rounded corners, socket flush with the top edge (checked against the footprint's own "PCB Edge" line and the 3D render), VBUS pads joined by a bottom-layer jumper on two vias, GND pour on the bottom layer.

## 4. Lessons carried into Phase 2

1. **Net-name mismatch.** The schematic-parity check warned because sheet-local nets are named `/GND` on the schematic side and `GND` on the PCB side. Rule for Franken-E58: power nets use global power symbols, never local labels (Block 5 rule 4).
2. **"Changed externally" refusal.** One save was refused because the file changed on disk, and was redone with force when nothing else had it open. Rule: the KiCad GUI is never open on a file the MCP server is editing, and every gate is committed to git first.
3. **Footprint edge guide.** The connector footprint carries its own "PCB Edge" line. Use it to place connectors flush with the board edge, and confirm in the 3D render.
4. **The test connector is power-only.** It has no D+ and D−. The Franken-E58 board needs a USB-C receptacle that brings out the data pins for the native USB port (Block 4 of the Phase 2 blocks).
5. **Live sync untested.** If wanted later: open KiCad with the API enabled, then run the server's connection check.

## 5. Blueprint diffs

None in P2-0. The Phase 2 diffs B-P2-01 to B-P2-10 are in `docs/Phase2_blueprint_blocks.md`.

## 6. Files

```
~/mcp-servers/KiCAD-MCP-Server/        (outside the repo)
~/.kicad-mcp/setup-progress.md         (outside the repo)
~/Documents/KiCad/usb-c-led-test/      (outside the repo; throw-away)
docs/P2-0_package.md
```

**P2-0 passed.**
