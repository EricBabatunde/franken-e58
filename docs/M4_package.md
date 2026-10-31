# Franken-E58 Phase 1, Module 4: VL53L1X Driver and Bench Test

**Scope:** a `drv_vl53l1x` component ported from the Pololu VL53L1X library (BSD-3-Clause), a `tof_task` on Core 0 that owns bus B and publishes readings through a seqlock slot, and a bench test covering static noise, accuracy, invalid cases, reset recovery and interference with the IMU loop. Also the blueprint updates from the M3 results (revision 2.6).

Documents used:
- **DS:** VL53L1X datasheet DS12385 rev 8.
- **UM2356:** VL53L1X API user manual (full API) rev 3.
- **UM2510:** VL53L1X ultra lite driver guide rev 7.

All three are in `docs/datasheets/`. The datasheet does not expose the register map (DS p.16), so register addresses and values come from the Pololu library. See `docs/THIRD_PARTY_LICENSES.md`.

---

## 1. Spec

### Part 0: blueprint updates (revision 2.6)
1. §2.3: rows "IMU timing" and "8 kHz mode" with the M3 results.
2. §4.4 item 2: the measured INT rates replace the "M3 measures" sentence. Option B is confirmed as the default; Option A stays a Phase 3 step 7 experiment because of the lost-pulse behaviour.
3. §4.3: ISR-to-task wake-up 7 µs average, 13 µs maximum (Wi-Fi off; M7 repeats it with Wi-Fi on).
4. §6.2: risk R6, accelerometer magnitude 1.068 g at rest.
5. §4.4 item 7: "about 40 Hz (20 ms timing budget, 25 ms inter-measurement period)". M4 row: "About 40 Hz ranging".

### Part 1: driver
- **Source:** Pololu vl53l1x-arduino, commit `8f21d608af00b42dc8776cfdf5ce935b45d2387d`, BSD-3-Clause.
  - Keep the library's logic: initialisation with the default configuration, distance mode, timing budget, continuous mode, result parsing and the raw-to-range status table.
  - Replace its I2C layer with `hal_i2c` and remove every Arduino dependency.
  - Cite the Pololu source file and function for each ported block.
  - Keep ST's and Pololu's notices in every derived file.
- **I2C:** all transfers go through `hal_i2c`, extended with `write_reg16` and `write_reg16_block`. Bus B (I2C1) is created by `tof_task` on Core 0.
- **Init sequence:**
  1. XSHUT reset;
  2. wait for boot with a timeout (`FIRMWARE__SYSTEM_STATUS`);
  3. check model ID 0xEACC;
  4. load the default configuration;
  5. short distance mode, timing budget 20 ms, inter-measurement period 25 ms (40 Hz);
  6. start ranging.
- **Reading:**
  - Poll data-ready; the GPIO1 interrupt is not used, because the GPIO interrupt service lives on Core 1.
  - Read the result, clear the interrupt, map the raw status.
  - An invalid reading is never published as a distance.
- **Publishing:** a seqlock slot (shared `components/seqlock`) carrying distance mm, valid flag, raw status, mapped status, signal rate, ambient rate, a 32-bit µs timestamp and a sequence number. Single writer: `tof_task` on Core 0, priority 6.
- **Memory and constants:** no heap allocation after init. Constants and limits live in `board_config`.

### Bench test (`src/m4_tof_test.cpp`, `FE_M4_TEST`)
`FE_M4_TEST` is mutually exclusive with `FE_M2_TEST` and `FE_M3_TEST` (compile-time `#error`). `FE_M4_PHASES` in `board_config.h` selects the phases: bit n runs Pn, P0 always runs, the default 0x3E runs P1 to P5. With all phases the run takes about 17 minutes; the start-up line prints the time for the selected phases. A countdown is printed whenever the target must move: 45 s before each P2 step, 15 s elsewhere.
- **P0:** init and ID check.
- **P1, 300 s static:** matte target at about 300 mm. Per 30 s window: rate, mean, standard deviation, min, max, valid % and a status histogram.
- **P2, accuracy sweep:** 100, 200, 400 and 800 mm, each with a 45 s countdown and 30 s of sampling. Every step ends on time even if all readings are invalid. Per step: mean, standard deviation, min, max, error against nominal ("n/a" with no valid reading), valid %. The SUMMARY gives error and valid % per step. 1200 mm was dropped: it is at the edge of the short-mode range (about 1.3 m).
- **P3, invalid cases:** 20 s covered by a hand at about 20 mm, then 20 s pointed at open space. Report the statuses and confirm no stale distance is published.
- **P4, reset recovery:** three XSHUT reset cycles while ranging. Time from release to the first valid reading.
- **P5, interference, 300 s:** M3 Phase B on Core 1 while the ToF runs at 40 Hz on Core 0. Compared with the M3 baseline: interval standard deviation 0.44 µs, jitter 3.7 µs, wake maximum 13 µs, burst average 450.4 µs, zero missed. P5 doubles as the M3 regression check.
- One SUMMARY line per phase (`SUMMARY Pn: skipped` for a phase not selected). At the end of the run, or after a P0 failure, every SUMMARY line is printed again in one block between `===== M4 SUMMARY RECAP =====` and `===== end of recap =====`.

### Report-only investigations
1. Default ROI and field of view, and how `SetROI` would narrow it for the Phase 4 mount.
2. Measured duration of the result read over I2C (bytes and µs).
3. Datasheet accuracy and noise figures for short mode, with page references.

## 2. Decisions

| ID | Decision | Reason |
|---|---|---|
| Licence | Port from Pololu vl53l1x-arduino (BSD-3-Clause), confirmed from the cloned `LICENSE.txt` at the commit above. The ULD route was dropped (no ST account) | Only licence verifiable from the primary file; ST's register map is not public |
| D-M4-01 | 40 Hz: timing budget 20 ms, inter-measurement period 25 ms | UM2356 p.5 and p.10: "The minimum inter-measurement period must be longer than the timing budget + 4 ms." The shortest budget is 20 ms, in short mode only (DS p.10, UM2356 p.9). 50 Hz would need a period of 20 ms and so a budget under 16 ms, which the documents do not allow for this driver. Not adopted |
| D-M4-02 | A distance is published as valid only for the library's "range valid" status. Every other status is invalid. The raw and mapped statuses are always published | User decision |
| D-M4-03 | Readings below 40 mm are also invalid | "The minimum ranging distance ensured is 4 cm" (DS p.21). **Phase 4 mount: the sensor must sit at least 40 mm above the ground when landed, or a landed drone reads invalid** |
| D-M4-04 | If no data-ready arrives for 3 periods (75 ms), `tof_task` publishes an invalid "no data" record | No stale distance is ever left looking current |
| D-M4-05 | P1 standard deviation is compared with 1 % repeatability (DS p.21), and P2 errors with ±20 mm (DS p.22, Table 19) plus 5 mm for the tape. Comparisons are printed for review, not as automatic failures | Datasheet figures are at a 100 ms budget (DS p.20); we run 20 ms |
| D-M4-06 | `FE_IMU_ISR_INSTRUMENT` is on for M3 or M4. The M3 test builds under either flag and offers `m3_phase_b_start(seconds)` | P5 reruns M3 Phase B |
| D-M4-07 | Header-only `components/seqlock` (`SeqlockSlot<T>`), used by `tof_task` and the M3 test | One implementation instead of two |
| D-M4-09 | **Long and Medium considered, not adopted; Short mode kept.** Can be reopened through a blueprint diff | (1) Under strong ambient light (200 kcps/SPAD) the maximum range is Short 135 cm, Medium 76 cm, Long 73 cm; in the dark Short 136 cm, Medium 290 cm, Long 360 cm (DS p.9, Table 5, measured at a 100 ms budget per the test conditions on DS p.10, so shorter at 33 ms). Short is the only mode that keeps its range in bright light. (2) 20 ms is the minimum budget and only in Short mode; 33 ms is the minimum for every mode (DS p.10). With the period longer than budget + 4 ms (UM2356 p.5) Medium or Long need a period of at least 38 ms: about 26 to 27 Hz instead of about 40 Hz. (3) The reading lags by about one budget: 33 ms instead of 20 ms in the altitude loop. (4) Longer reach gives more far-field "range valid" ghost readings (R7). (5) P1 to P5 would be rerun and the P2 accuracy re-measured. The 11:59 run on 31 October (all readings out of bounds) was a test-setup issue, not a reason to change mode |
| D-M4-10 | `board::kTofDistanceMode` (default Short), applied in `drv_vl53l1x::init()` through `set_distance_mode()`. Compile-time checks: budget at least 20 ms in Short mode and 33 ms in Medium or Long (DS p.10); period strictly longer than budget + 4 ms (UM2356 p.5, "longer than") | A mode change is one constant plus a budget and period that the build checks |
| D-M4-08 | Inter-measurement period register = `period_ms × (OSC_CALIBRATE_VAL & 0x3FF) × 1.075` (ST `VL53L1X_SetInterMeasurementInMs()`, SparkFun BSD-3 copy), replacing Pololu's `period_ms × OSC_CALIBRATE_VAL` | First bench run: Pololu's formula gave 42.63 Hz, a real period of 23.46 ms for 25 ms, below budget + 4 ms (UM2356 p.5). Acceptance 3 unchanged |

## 3. Acceptance criteria
1. Clean build and `tools/verify_sdkconfig.sh` passes.
2. Model ID 0xEACC, boot and init succeed.
3. Ranging rate 40 ± 2 Hz over P1.
4. P1 valid readings at least 99 %, with the standard deviation reported and compared with the datasheet.
5. P2 mean error per step reported and compared with the datasheet figure. Flag any step above the datasheet figure plus 5 mm for tape-measure error (25 mm). For review, not an automatic failure.
6. P3 shows invalid statuses reported as invalid, never as a stale distance.
7. P4 recovers all three times. Report the times.
8. P5: the IMU shows zero missed samples, interval standard deviation below 2 µs, jitter below 20 µs and wake maximum below 50 µs.
9. The BSD-3 licence is confirmed from the cloned files and documented in `docs/THIRD_PARTY_LICENSES.md`.

## 4. Bench procedure
1. **Wiring:** as M2 (`docs/M2_package.md` §2), with the IMU wired as for M3. XSHUT is GPIO 8.
2. **Flags:** `FE_M4_TEST` 1, `FE_M2_TEST` and `FE_M3_TEST` 0. Build is clean and `tools/verify_sdkconfig.sh` passes.
3. **Equipment:** a matte target, such as grey card or cardboard larger than the field of view (about 27° full cone, so roughly 15 cm wide at 300 mm), and a tape measure. Measure from the sensor's front face. Avoid sunlight on the sensor.
4. **Flash and monitor.** Start the monitor from the project folder: the `log2file` filter saves everything to `logs/device-monitor-YYMMDD-HHMMSS.log` (ignored by git), including any reset or backtrace. Follow the `>>>` prompts; each counts down before measuring (45 s for P2 steps, 15 s otherwise). To repeat one phase, set `FE_M4_PHASES` (for example 0x04 for P2 only), rebuild and flash.
   - **P1:** hold the target still at 300 mm for 5 minutes. A clamp or stack of books is better than a hand.
   - **P2:** move the target to 100, 200, 400 and 800 mm when prompted.
   - **P3:** cover the sensor with your hand at about 20 mm, then point it at open space with nothing within 1.3 m. A ceiling higher than 1.3 m works.
   - **P4:** hold the target at about 300 mm.
   - **P5:** keep the board still for 5 minutes, with a target (for example a stack of books) within about 300 mm of the sensor. With nothing within about 1.3 m, Short mode reports every reading as out of bounds and P5 shows ToF valid 0 %. The `m3` lines print the IMU windows; the P5 SUMMARY line ends with the ToF status breakdown.
5. **Copy back:** the recap block, the P2 step lines, the P4 lines, the M3 `SUMMARY B` and `SUMMARY B checks` lines, and any error or warning. If the output stops, send the end of the newest log file.

## 5. Files

```
components/drv_vl53l1x/      (BSD-3-Clause, derived from Pololu vl53l1x-arduino)
├── CMakeLists.txt
├── include/drv_vl53l1x.h
└── drv_vl53l1x.cpp
components/seqlock/
├── CMakeLists.txt
└── include/seqlock.h
src/
├── tof_task.h, tof_task.cpp
├── m4_tof_test.h, m4_tof_test.cpp   (bench test, deleted before Module 7)
docs/THIRD_PARTY_LICENSES.md
```

Also changed:
- `hal_i2c`: `write_reg16`, `write_reg16_block`.
- `board_config.h`: ToF constants, distance mode and its compile-time checks (D-M4-10), test flags, `FE_M4_PHASES`, `FE_IMU_ISR_INSTRUMENT`.
- `src/m3_imu_test.*`: shared seqlock, Phase-B-only entry point.
- `src/main.cpp`, `src/CMakeLists.txt`, `.gitignore` (`docs/third_party/`, `logs/`).
- `platformio.ini`: `log2file` monitor filter.
- `docs/THIRD_PARTY_LICENSES.md`: ST period formula (D-M4-08).

## 6. Results (bench, 30 October 2026): M4 passed

Log files are in `logs/` (not in git). The P1 run predates the `log2file` filter, so its figures come from the copied terminal output.

### Measured results

| Phase | Log | Result |
|---|---|---|
| P0 | every good run | Model ID 0xEACC, boot 308 to 320 µs after XSHUT release |
| P1 (first run, before D-M4-08) | terminal copy | 42.63 Hz, valid 100 %, mean 283.0 mm, sd 1.50 mm, 17-byte result read min/avg/max 540/541/554 µs, 0 no-data, 0 missed records. The stale count of 12791 was the double-count bug, fixed afterwards |
| P2 | `device-monitor-261030-014016.log` | 100 mm: +3.6 mm (sd 2.23); 200 mm: −17.8 mm (sd 1.79); 400 mm: −10.7 mm (sd 1.58); 800 mm: +16.4 mm (sd 2.09). Valid 100 % at every step, 0 flagged above 25 mm. The 1200 mm step was dropped: it is at the short-mode limit of about 1.3 m |
| P3 | `…014707`, `…014932`, `…015143` | Stale distances 0 in all three runs (PASS). Hand: 287, 0 and 0 valid readings (the first attempt probably had the hand above 40 mm for part of the run). Open space: 0, 40 and 28 valid of 795 (0 %, 5.0 %, 3.5 %), the rest "out of bounds fail" |
| P4 | `…015527` | 3/3 recovered. XSHUT release to first valid reading 44.2, 43.2 and 43.2 ms; boot 186 to 212 µs |
| P5 | `…015753` | IMU interval sd 0.46 µs, jitter 3.7 µs, wake max 12 µs, burst average 450.5 µs, 0 missed, 0 timeouts, 0 I2C errors, 999.70 Hz. ToF 39.68 Hz, valid 100 %, 0 no-data |

### Acceptance

| # | Criterion | Result |
|---|---|---|
| 1 | Clean build, `tools/verify_sdkconfig.sh` | PASS |
| 2 | Model ID, boot, init | PASS |
| 3 | 40 ± 2 Hz over P1 | PASS on the P5 evidence: 39.68 Hz over 300 s at the same ToF settings. Deviation: the P1 rerun after D-M4-08 could not complete because of the bus B wiring fault below, so the rate is taken from P5 (option A, agreed). P1 before the fix measured 42.63 Hz, the cause of D-M4-08 |
| 4 | P1 valid ≥ 99 %, sd reported | PASS: valid 100 %, sd 1.50 mm against 2.8 mm (1 % of the mean, DS p.21, 100 ms budget) |
| 5 | P2 errors reported, flag above 25 mm | PASS: largest error 17.8 mm, none flagged |
| 6 | P3 invalid statuses never published as a distance | PASS: 0 stale in all runs |
| 7 | P4 recovers 3 times | PASS: 43 to 44 ms |
| 8 | P5 IMU limits | PASS |
| 9 | BSD-3 licence confirmed | PASS (`docs/THIRD_PARTY_LICENSES.md`) |

### Failed P5 rerun (not the M4 result)

A later P5 rerun (`device-monitor-261030-222201.log`) ran after the bus B connection to the ToF had failed (bad solder joints, see "Fault and fix" below). The ToF dropped out: 0.05 Hz, valid 21.4 %, 3,582 no-data records over 300 s. The IMU figures were unaffected: interval sd 0.47 µs, jitter 3.7 µs, wake max 16 µs, burst average 450.4 µs, 0 missed. M4's pass rests on the earlier clean P5 run (ToF 39.68 Hz, 100 % valid).

### Heap during P5

Every P5 run shows the same −220 B change: 8701624 B to 8701404 B in both 30 October runs, and 8701088 B to 8700868 B in both 31 October runs (`…115903`, `…121248`; the start figure moved with the C1 code). These totals include PSRAM. The heartbeat lines show internal free heap dropping by 220 B once, between t = 10 s and 11 s, when M3's first 10 s window prints, and then staying flat. It is a one-time step, not a drift. `SUMMARY B checks` now reports heap start, end and change as information instead of a PASS/FAIL "heap unchanged" check.

### P0 failures after the wiring fault

From 02:16 on 30 October, 37 boots failed P0 with the same signature: `init step 2 (ESP_ERR_TIMEOUT)`, model ID 0x0000. The sensor never reported "booted" within 100 ms of XSHUT release. No IDF I2C error lines appeared during these boot polls; IDF 5.5.3 logs a NACK only at debug level but a bus timeout at error level, so the polls were most probably NACKed. Boots that had already passed P0 later logged "I2C transaction timeout detected" and then delivered only no-data records. Follow-ups:
- **C1 (approved):** boot-time SDA/SCL check and 9-clock bus clear on both buses, plus ToF boot diagnostics in P0.
- **C2 (proposal, later module):** `tof_task` re-initialises the sensor through XSHUT after N silent periods, with an attempt limit.

Resolved on 31 October: see "Fault and fix" below.

### C1: bus line check and ToF boot diagnostics (added after the M4 pass)

- **Line check before each bus is created (`hal_i2c::init()`):** SDA and SCL are read. SDA low with SCL high triggers up to 9 SCL clocks and a STOP; SCL low is recorded and not clocked. The outcome is kept per bus (`hal_i2c::line_check()`) and printed from Core 0: bus B in the P0 line, bus A by the M3 reporter (M3 and M4 P5). `recover()` still calls `bitbang_clear()` directly and records nothing; `bitbang_clear()` now returns its clock count and is otherwise unchanged.
- **ToF boot diagnostics (`drv_vl53l1x::init()`, `InitStatus::diag`):**
  - SDA/SCL/XSHUT levels (a) before the XSHUT pulse, (b) at its middle, (c) 10 ms after release, (d) at the end of a failed boot poll. Printed on one P0 line.
  - Boot poll count, last I2C error and last status byte. IDF 5.5.3 returns `ESP_ERR_INVALID_STATE` for both a NACK and a bus timeout (`i2c_master.c:726`), so the scan is what separates them.
  - On a failed boot poll only: one scan of bus B (0x08 to 0x77; `ESP_ERR_NOT_FOUND` = NACK, `ESP_ERR_TIMEOUT` = bus timeout; stops after 3 timeouts in a row), then one retry with XSHUT low 100 ms and a 200 ms boot wait. The retry is a diagnostic: P0 still fails.
  - `start_continuous` is printed only when init succeeded.
- **Side effect:** (c) waits until 10 ms after release even on a healthy boot, so every init takes up to 10 ms longer. P4's "release to first valid reading" will read about 10 ms longer than the M4 results above (43 to 44 ms).

### Fault and fix (31 October 2026)

- **Root cause:** bad solder joints on the VL53L1X breakout's SDA and SCL pins. Fixed by resoldering both pins.
- **How it was found:**
  - The P0 failures above all had one signature: boot poll timeout, model ID 0x0000.
  - The C1 diagnostics showed SDA and SCL both low before bus B was created, when the ESP32 drives neither line, and the scan stopped after three bus timeouts. Bus A read idle-high throughout.
  - Meter checks: continuity passed and nothing was shorted with the power off (SDA and SCL about 4.7 kΩ to GND and about 3.3 to 3.5 kΩ to 3V3, the breadboard 4.7 kΩ pull-ups in parallel with the breakout's own). Powered, SCL sat at about 0.4 to 0.7 V even with the ESP32 held in reset. A 2 s power cycle of the breakout did not clear it.
- **After the fix:** `bus B lines before creation: SDA 1 SCL 1, idle, no clear`, model ID 0xEACC, init OK. The module now boots 1.18 ms after XSHUT release (186 to 320 µs before), still within the 1.2 ms maximum (DS p.11).
- **Not related:** an unrelated Arduino sketch was briefly flashed during the investigation. It reads GPIO 8 and 9, which on this board are the ToF XSHUT and the IMU INT line, so its "SCL held low" message was false. It is irrelevant to this build.

### Confirming P5 after the fix (31 October 2026): M4 passed

`device-monitor-261031-121248.log`, `FE_M4_PHASES 0x20`, with a target (a stack of books) in front of the sensor:

| Item | Result |
|---|---|
| P0 | PASS: bus B lines idle-high, model ID 0xEACC, boot 1178 µs |
| ToF | 39.65 Hz, valid 100 %, 0 no-data |
| IMU | Interval sd 0.49 µs, jitter 3.6 µs, wake max 12 µs, burst average 450.5 µs, 0 missed, 0 timeouts, 0 I2C errors, 999.64 Hz (all within the acceptance 8 limits) |
| Heap | −220 B, the same one-time step |

The earlier run that morning (`…115903`) showed ToF 39.71 Hz but valid 0.0 %: nothing was within Short mode's range of about 1.3 m, so every reading was "out of bounds". That is the designed behaviour (no stale distance was published), not a fault; a stack of books in front of the sensor fixed it. P5's SUMMARY line now ends with the ToF status breakdown, so such a run shows the cause directly.

**M4 passed.** The pass rests on the 30 October results and is confirmed by this run after the wiring fix.
