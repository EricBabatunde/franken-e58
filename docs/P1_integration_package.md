# Phase 1 integration run (stage 1 of the Phase 1 close-out)

**Status:** **passed 9 November 2026**: acceptance 1 to 5 (§9, §10). Blueprint revision 2.13 holds the four approved diffs; the integration row is filled in.

One run with everything together:
- the flight loop and every M7 safety feature;
- the UDP and WebSocket comms and page serving;
- the ToF sensor on bus B;
- the 150 µs synthetic load.

The ToF hasn't been read since M4 (§1.1).

## 0. Safety boundary
- No motors, MOSFET stage or shield. 10 kΩ pull-downs from GPIO 4 to 7 to GND, multimeter only.
- Bench duty cap 200 permille. The banner reads "P1 integration bench firmware: no motors may be connected".
- Every M7b and M7c protection stays active.
- The console keys stay on UART0 and none can arm or drive an output. The new keys `5` and `6` only label conditions.
- **Nothing from the ToF feeds the safety logic (§1.4).**

## 1. Findings before the run

### 1.1 Which bench builds started `tof_task`
Only M4's test (`src/m4_tof_test.cpp:283`). Checked in the source and at every module commit (`a5ba193` M5, `fe21ec9` and `7e1df9e` M6, `c2fd2a5` M7a, current tree for M7b and M7c): the M5, M6, M7a, M7b and M7c benches never call `tof_task_start()`. So the ToF has never run together with Wi-Fi, page serving or the M7 safety features.

### 1.2 Core, priority and the bus B interrupt
- **`tof_task`:** Core 0, priority 6, stack 4096 B (`src/tof_task.cpp`). That's above `httpd` and `udp_rx` (5) and below `tiT` (18), `esp_timer` (22) and `wifi` (23).
- **Bus B's I²C interrupt:** `tof_task` creates bus B itself (`hal_i2c::init(Bus::B, …)`), and the IDF I²C master driver allocates the interrupt on the core that creates the bus, so it should be on Core 0 (verify against IDF docs).
- **ToF INT pin (GPIO 13):** not used; the task polls data-ready.
- **Confirmed at run time:** the integration build starts the ToF before the task table and the interrupt dump, so the table shows `tof` on core 0 and the dump shows `I2C_EXT1` under CPU 0. The analyser reports both.

### 1.3 Locks on the ToF path
All on Core 0:
- bus B's bus lock inside the IDF `i2c_master` driver;
- that driver's interrupt-to-task queue;
- the FreeRTOS kernel lock behind `vTaskDelay` and the queue calls: the known, unavoidable one.

Lock-free or Core 0-only:
- the readings and the status use seqlock slots (`SeqlockSlot<TofSample>`, `SeqlockSlot<TofStatus>`);
- the reset request is a Core 0-only `std::atomic<bool>`;
- `hal_i2c` keeps a separate state per bus (`g_bus[2]`), with no lock shared between the buses.

Nothing on the ToF path shares a user-level lock with Core 1.

Since M7c the I²C master code runs from IRAM (`CONFIG_FE_R5_I2C_IRAM`), so bus B transfers do too.

### 1.4 ToF and the safety logic
- `components/flight`, `components/safety`, `components/comms`, `components/wdt` and `components/hal_mcpwm` contain no reference to the ToF (`tof`, `Tof`, `vl53`, `VL53`): source search, 0 hits.
- `libflight.a` and `libsafety.a` contain no ToF symbol (`nm`, bench build).
- The only reader of the ToF records is the Core 0 reporter `src/p1_integration.cpp`.

So ToF state cannot cause a cut, a refusal or an arm.

## 2. Spec as implemented

### 2.1 Decisions (approved 9 November 2026)
| ID | Decision |
|---|---|
| D-P1-1 | `TofStatus` gains cumulative counters, updated and published with every record: readings, valid readings, no-data records, completed reads per range status (16 slots), the time of the last reading, the first no-data record after it, the number of no-data episodes, and the latest distance. A reporter can then work out per-window rates without seeing every record |
| D-P1-2 | After a bus B I/O error, `tof_task` waits one period (`board::kTofErrorBackoffMs` = 25 ms) before polling again, cut short at the no-data deadline. Without it, an unplugged SDA fails every 2 ms poll and the IDF driver logs every failure (about 500 lines a second). The no-data deadline (75 ms) is unchanged. This also applies to the M4 test |
| D-P1-3 | `docs/PHASE1_REPORT.md` is pre-filled from the module docs, with sources cited. Placeholders where a doc records no figure |
| — | The synthetic load is on from boot in this build (`L` still switches it). Keys `5` and `6` start conditions `i1` and `i2` |

### 2.2 Firmware
- **Flag:** `FE_P1_INTEGRATION` (`board_config.h`). It requires `FE_M7C_TEST`, or the build stops with an error.
- **Start order:**
  1. outputs, network and the flight task, as in M7c;
  2. `p1_start()` starts `tof_task` and waits up to 300 ms for it to leave the starting state;
  3. the `TOF:` boot line: PASS or FAIL, state, bus B lines, init result, task core and priority;
  4. the synthetic load is switched on;
  5. the task table and `esp_intr_dump()`.
- **Every 10 s, a `TOF wNN` line:** rate, readings, valid percentage, no-data records, bus B I/O errors (window and total), the range statuses seen, the latest distance and its age, the task state, inits, no-data episodes. It sits beside the existing `B`, `loop`, `wdt` and `HB` lines.
- **Event lines:**
  - `TOF EVENT no-data`: the last reading's time, the first no-data record's time, and the gap between them. The gap is ≥ 75 ms by design.
  - `TOF EVENT readings resumed`: printed if readings come back without a re-initialisation.
- **The `tof` task state:** it stays `running` while the bus fails, because there's no re-initialisation (R8). The no-data records and the I/O error count show the failure.

### 2.3 Tools
`tools/analyse_m7b.py` has a new Phase 1 section:
- boot facts: reset reason, brownouts, `TOF:` result, the task-table entry, and which CPU `I2C_EXT1` is on;
- P1-2 to P1-5 for the latest finished `i1` and `i2` conditions, with ToF windows, cuts, arm cycles and heap taken between `TM START` and `TM … END`.

It was checked on a synthetic log built from the firmware's own format strings.

## 3. Acceptance
1. **Builds:** all environments build cleanly, and `tools/verify_sdkconfig.sh` passes for each.
2. **ToF during I1:** 40 ± 2 Hz, valid ≥ 99 %, no-data records 0.
3. **IMU as in M7c:**
   - loop-period p99.9 < 100 µs;
   - wake-up max < 50 µs;
   - 0 missed samples;
   - 0 I²C errors on both buses during I1: bus A from the `TM` line, bus B from the `TOF` lines.
4. **No unintended cuts or resets:**
   - the only cut allowed is the per-cycle `link_silence`;
   - heap drift within 2 KB;
   - reset reasons and brownouts listed.
5. **I2:**
   - IMU figures as in 3;
   - arm cycles and their link-loss cuts continue with no other cut;
   - the ToF reports no-data within 75 ms of the unplug.

   **How the 75 ms is checked:** the firmware can't see the moment of the unplug. It measures from the last reading (at most one 25 ms period before the unplug) to the first no-data record, and declares no-data 75 ms after the last reading. So the analyser accepts a gap of up to 90 ms: 75 ms, plus one failed 10 ms bus B transfer, plus the 2 ms poll.

   **What happens after reconnecting** is recorded: either `TOF EVENT readings resumed`, or no readings until reset.

## 4. Run procedure
**Set-up:**
- Board flat and still, on the short USB cable straight into the laptop.
- ToF target (a book stack) at about 300 mm.
- 10 kΩ pull-downs on GPIO 4 to 7, no motors.
- The laptop's background Wi-Fi scan off (§8b of `docs/M7c_package.md`).

**Steps:**
1. **Build and check:** `pio run`, then the config check, as in §5.
2. **Flash:** `pio run -e esp32s3_bench -t upload`. Monitor on UART0.
3. **Boot checks:**
   - the P1 banner;
   - `TOF: PASS | state running … task core 0 priority 6`;
   - `P1 integration: synthetic load ON`;
   - `TASK tof … core 0 prio 6/6`;
   - `I2C_EXT1` under `CPU 0 interrupt status` in the dump;
   - `FLIGHT: PASS`, `WDT: PASS`, `TASKS: PASS`;
   - no `BROWNOUT` in the `RESET:` line.
4. **Join the laptop to the AP.**
5. **I1, 10 minutes:**
   1. Press `5`.
   2. Within a few seconds, start in three terminals:
      - `.venv/bin/python tools/run_m7b.py --transport udp --minutes 10`
      - `.venv/bin/python tools/latency_test.py --transport ws --no-control --count 30000`
      - `.venv/bin/python tools/m5_http_test.py --load 600`
   3. When all three have finished, press `n`, then `s`.
6. **I2, 5 minutes:**
   1. Press `6` and start:
      - `run_m7b.py --transport udp --minutes 5`
      - `latency_test.py --transport ws --no-control --count 15000`
      - `m5_http_test.py --load 300`
   2. About 1 minute in, unplug the ToF SDA wire for 30 s, then reconnect it. Note both times.
   3. When the tools finish, press `n`, then `s`.
   4. Expect `TOF EVENT no-data` with a gap of 75 to 90 ms, the bus B I/O errors rising during the 30 s, and the arm cycles continuing.
   5. Note whether `TOF EVENT readings resumed` appears after reconnecting.
7. **Optional:** power-cycle and check `TOF: PASS` again.
8. **Analyse:** `python3 tools/analyse_m7b.py logs/device-monitor-<...>.log`, with every log of the run in order. Send the output and the three tool summaries.

**WebSocket ping losses are expected.** The `latency_test.py` summaries will show heavy loss under page serving, because the WebSocket replies queue behind page sends (B-M7c-09). Since M7c that's reported as a FAIL. It isn't part of this acceptance, and the rule against page serving while ARMED (revision 2.13) is the fix.

## 5. Development results (9 November 2026, no hardware)
**Recorded at the end of stage 1 (§7):**
- the builds and config checks;
- the host tests;
- the symbol checks;
- the analyser check on synthetic data.

## 6. Not testable without hardware
- **Every figure in acceptance 2 to 5**, and the core actually given to the bus B interrupt.
- **The supply margin with the ToF connected:** the ToF adds current while ranging, and the supply had little headroom in M7c.
- **What the ToF and the IDF I²C driver do after SDA is reconnected.**

## 7. Stage 1 results
- **Builds:** all eight environments, 0 warnings. `verify_sdkconfig.sh`:
  - `esp32s3` 39/39;
  - `esp32s3_bench` 41/41;
  - `r5b` 41/41;
  - `r5c`, `r5d`, `r5e` 42/42 each;
  - `heapdiag` and `heapdiag_nommap` 43/43 each.
- **Host tests:** safety 7 455 485 checks, output gate 566 535, seqlock PASS, control frame 4043, latency histogram 7211, all 0 failed. `latency_test.py --selftest` 14/14.
- **Safety isolation (§1.4):** 0 source references to the ToF; 0 ToF symbols in `libflight.a` and `libsafety.a`.
- **Core 1 libraries:** no log, printf, heap, guard or atomic library calls, only `abort` (key `X`).
- **Analyser:** on the synthetic log it reported the boot facts (TOF PASS, core 0, priority 6, `I2C_EXT1` on CPU 0), P1-2 to P1-4 for I1, and P1-5 for I2, including the unplug gap and "stays dead until reset".

## 9. Bench results: I1 (9 November 2026)
Logs: `logs/device-monitor-261109-054100.log` and its continuation `…261109-055510.log`. The monitor was stopped and restarted, so about 200 s of output between 646 s and 847 s isn't logged. There was no reset: same boot, uptime continuous. One POWERON boot, brownouts 0.

| # | Result |
|---|---|
| Boot | `TOF: PASS`; task `tof` on core 0, priority 6; **bus B interrupt `I2C_EXT1` on CPU 0** |
| P1-2 | **PASS.** ToF 39.71 Hz over the 630 s of logged `TOF` lines, valid 100.00 %, no-data 0, bus B I/O errors 0. Latest distance 283 mm |
| P1-3 | **PASS.** Period p99.9 12 µs (max 27 µs), wake-up max 17 µs, 0 missed, 0 I²C errors on bus A and bus B. Load 150 µs |
| P1-4 | **PASS.** 28 arm cycles with 28 expected link-loss cuts, no other cut, no reset, no brownout. Internal heap 170 563 B idle before, 170 447 B when the traffic stopped (−116 B), 172 627 B at the end (+2064 B, a session's buffers released); minimum 121 251 B under load |
| Timing | Under the full load (60 s line at t = 608 s): burst average 483.9 µs; loop work average 644.8 µs, p99.9 689 µs, max 699 µs. **Average margin 355 µs (target ≥ 350); worst-frame margin 301 µs (target ≥ 250)** |

**Two notes:**
- **Analyser heap rule:** it first reported P1-4 as FAIL on the +2064 B. The rule judged the absolute drift, but more free heap at the end is memory given back, not a leak. It now fails only on a loss of more than 2 KB, for the soak and the P1 runs alike, and the M7c soak still passes.
- **WebSocket pings:** 78 of 30 000 answered within 200 ms (`latency_test.py`: FAIL, loss 99.74 %), as expected under page serving (B-M7c-09). Not part of this acceptance.


## 10. Bench results: I2 (9 November 2026)
Log `logs/device-monitor-261109-060321.log`: one POWERON boot, brownouts 0, same build as I1.

| # | Result |
|---|---|
| P1-5 IMU | **PASS.** Period p99.9 13 µs (max 25 µs), wake-up max 17 µs, 0 missed, 0 bus A I²C errors, burst average/max 483.4/530 µs, loop work average/max 644.2/694 µs, load 150 µs, 313 s |
| P1-5 arming | **PASS.** 14 arm cycles with 14 expected link-loss cuts, no other cut, no reset, through the unplug. Heap drift −232 B |
| P1-5 ToF | **PASS.** Last reading at 110.977 s; first no-data record at 111.064 s, a **gap of 87.0 ms** (deadline 75 ms, accepted ≤ 90 ms). One no-data episode. 1107 bus B I/O errors during the unplug, about 32 a second with the D-P1-2 back-off instead of about 500. No I²C driver error lines flooded the console |
| After reconnecting | **Readings resumed by themselves**, without a re-initialisation: first reading at 145.696 s (valid, 279 mm), inits still 1. The ToF was silent for about 34.7 s for the 30 s unplug |

**Findings:**
- **The ToF recovers from an SDA interruption on its own.** The sensor keeps ranging in continuous mode, and the IDF I²C driver recovers after the bus errors. This is better than expected ("stays dead until reset"). R8 still stands: a power loss or brown-out of the sensor, or a hung sensor, needs re-initialisation through XSHUT. The run showed nothing about those cases.
- **Page serving starves the low-priority Core 0 tasks:**
  - 64 heartbeat lines (1 s period) in about 340 s;
  - the 10 s `TOF` lines came between 0.1 s and 22 s apart.

  The counters are cumulative and each rate uses the real elapsed time, so the figures are correct. The reporter now schedules its next line from the time it prints, so a late window no longer causes catch-up lines (`src/p1_integration.cpp`, bench only; built, 0 warnings).
- **Burst spikes of 1123 to 1240 µs** in the first 10 s after boot are the Wi-Fi PHY calibration (B-M7c-10), before any condition. Inside I2 the burst max was 530 µs.
- **WebSocket pings:** 103 of 15 000 answered in time (B-M7c-09), as expected.

## 8. Files
```
src/p1_integration.h, src/p1_integration.cpp                                  (new)
src/tof_task.h, src/tof_task.cpp              (D-P1-1 counters, D-P1-2 back-off)
src/m7b_bench.cpp      (p1_start before the task table, p1_poll, banner, "tof" in the task names)
src/m7c_timing.h, src/m7c_timing.cpp          (keys 5 and 6, m7c_set_load)
src/CMakeLists.txt
components/board_config/include/board_config.h (FE_P1_INTEGRATION, kP1Banner, kTofErrorBackoffMs)
tools/analyse_m7b.py                          (Phase 1 section, P1 banner)
docs/P1_integration_package.md, docs/PHASE1_REPORT.md                        (new)
docs/Franken-E58_Full_System_Architecture_Blueprint.md (revision 2.13)
CLAUDE.md                                     (status line)
```
