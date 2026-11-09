# Phase 1 report: breadboard prototyping and telemetry validation

**Status: Phase 1 complete (9 November 2026; blueprint revision 2.14).** Filled in after the integration run (`docs/P1_integration_package.md`). The remaining `[PLACEHOLDER]` items are open inputs for Phase 2 (part numbers, current draw, VBAT sensing, brownout level) or data that was never recorded (marked "not recorded").
- Figures come from the module docs and the blueprint, with their sources cited.
- Nothing is estimated.
- **`[PLACEHOLDER]`** marks data that is missing or not yet measured.

## 1. Summary
Phase 1 validated every breadboard module (M1 to M7c), then ran everything together in one integration run.

**Integration run** (flight loop, M7 safety features, UDP and WebSocket comms, page serving, the ToF on bus B, and the 150 µs stand-in for the control maths):
- The 1 kHz IMU loop kept loop-period jitter p99.9 at 12 to 13 µs and wake-up under 20 µs, with zero missed samples and zero I²C errors on both buses.
- The ToF ran at 39.7 Hz with 100 % valid readings.
- Arming and the link-loss cuts behaved exactly as specified.

**Margins** (with the IMU's I²C transfer in IRAM, adopted in M7c):
- average 355 µs, against a target of ≥ 350 µs;
- worst frame 301 µs, against ≥ 250 µs.

Both must be re-measured once the real estimator, PID and mixer exist.

**Open before motors run under load:** the O4 power-protection gate (VBAT sensing and brownout tuning).

**Biggest practical finding:** supply robustness. Every crash and brownout of the phase went away with a better USB cable.

## 2. Modules M1 to M7c and the integration run
The M1 to M3 bench runs predate the `log2file` monitor filter (first saved log: 30 October 2026), so their logs weren't kept. Their figures are those recorded in blueprint §2.3.

| Module | Scope | Measured results | Proof (logs, docs) | Passed |
|---|---|---|---|---|
| M1 | PlatformIO skeleton, logging, GPIO 4 to 7 low at boot, LED probe | Flash 16 MB generic chip, DIO at 80 MHz (QIO enable failed); PSRAM 8 MB octal at 40 MHz; dual core 240 MHz, chip v0.2; UART0 and native USB Serial/JTAG (`303a:1001`) both log; power-on reset, no brownout (blueprint §2.3) | Logs not kept; `docs/M1_package.md`; commit `c300083` | 28 October 2026 (commit) |
| M2 | Dual-bus I²C scan, IDs, recovery, 10-minute soak | Both buses at 400 kHz, 10-minute soak with zero errors on both (blueprint §2.3). Recovery self-test result: not recorded (monitor output not saved before 30 October 2026) | Logs not kept; `docs/M2_package.md`; commit `c83ce16` | 28 October 2026 (commit) |
| M3 | MPU-6500 driver, 1 kHz interrupt-driven sampling | 999.74 Hz, interval sd 0.44 µs, ISR-to-task wake-up 6 to 13 µs, 14-byte burst 450 µs, 10 minutes with zero missed samples. DLPF_CFG 0: 7997.9 Hz data-ready, occasional lost raw pulse under read traffic (blueprint §2.3) | Logs not kept; `docs/M3_package.md`; commit `4bc4e07` | 29 October 2026 (commit) |
| M4 | VL53L1X driver, `tof_task` on bus B | 39.68 Hz, valid 100 %, sd 1.50 mm at about 300 mm; errors +3.6 / −17.8 / −10.7 / +16.4 mm at 100 / 200 / 400 / 800 mm; recovery 43 to 44 ms (3 times); IMU unaffected alongside (P5: sd 0.46 µs, jitter 3.7 µs, wake-up max 12 µs, 0 missed) | `logs/device-monitor-261030-014016.log` (P2), `…261030-015753.log` (P5), `…261031-121248.log` (P5 after the wiring fix); `docs/M4_package.md` §6 | 30 October 2026, confirmed 31 October |
| M5 | LittleFS, Wi-Fi AP, static pages, IMU under Wi-Fi | 200/200 GET `/`, 20/20 checksums; heap after load −852 B (minimum 159 959 B); Wi-Fi, lwIP and httpd on Core 0; IMU within limits; burst 451 µs idle, 605 to 633 µs under page load (R5) | `logs/device-monitor-261102-193700.log`; `docs/M5_package.md` §8 | 2 November 2026 |
| M6 | WebSocket and UDP latency harness | One-way p95/p99: laptop UDP 2.00/2.59 ms, laptop WebSocket 3.86/5.26 ms, phone WebSocket 4.90/8.25 ms. IMU whole run: sd 0.51 µs, jitter 13.6 µs, wake-up max 19 µs, 0 missed in 1 199 627 samples. 0 torn setpoints; heap +28 B after 60 000 frames | `logs/device-monitor-261103-150125.log`, `…261103-151835.log`; `docs/M6_package.md` §10 | 3 November 2026 |
| M7a | Motor outputs (forced low), arming and failsafe state machine | Link loss: 40 trials, decision 150.02 to 150.97 ms, force-low 1.48 µs after the decision. Kill 991 µs to force-low. Missing data-ready 4031 µs. Meter 0.07 V at throttle 100, 0.65 V at 1000, 0 V disarmed and after cuts. IMU under traffic: 0 missed, wake-up max 15/17 µs (UDP/WebSocket) | `logs/device-monitor-261104-052501.log`, `-053851`, `-054419`, `-064232`; `docs/M7a_package.md` §9 | 4 November 2026 |
| M7b | GPTimer watchdog and hard latch, panic force-low, interrupt and task watchdogs, IMU recovery, boot retry | Heartbeat: 22 cuts, 5008 to 5465 µs. Data-ready: 42 cuts, 3017 to 3496 µs. Force-low ≤ 1.91 µs. `H`/`T`/`X` reset 1.40–1.44 / 3.21 / 1.21 s after the hang, reboot DISARMED, 0 V during backtraces. 22 resets while ARMED, all back to DISARMED. 67 recoveries OK in about 104 ms. 688 s ARMED under traffic, no cut | `logs/device-monitor-261106-194011.log` to `…261107-035524.log` (20 files); `docs/M7b_package.md` §8b | 7 November 2026 |
| M7c | Timing under load, R5 experiments, soak | Jitter p99.9 4/6/7/12 µs (idle, UDP, WebSocket, page), wake-up max ≤ 21 µs, 0 missed. With the 150 µs load: p99.9 9 µs, loop work max 929 µs. R5 (b) adopted: page-load burst 620 → 479 µs, worst frame 783 → 553 µs. Soak 1267 s: 40 arm cycles, 0 missed, heap drift −112 B | `logs/device-monitor-261107-135922.log`, `-154912`, `-161107`, `-170547`, `-232235`, `-233517`, `…261109-025859.log`, `-032844`, `-034111`; `docs/M7c_package.md` §8d | 9 November 2026 |
| P1-INT | Integration: everything together, ToF on bus B, 150 µs load | I1: ToF 39.71 Hz, valid 100 %, no-data 0; period p99.9 12 µs, wake-up max 17 µs, 0 missed, 0 I²C errors on both buses; 28 arm cycles, no unintended cut, no reset or brownout; heap −116 B at the end of the traffic; loop work avg/max 644.8/699 µs. I2 (ToF SDA unplugged for 30 s): IMU unchanged (p99.9 13 µs, 0 missed), 14 arm cycles unaffected, no-data 87 ms after the last reading, readings resumed by themselves after reconnecting | `logs/device-monitor-261109-054100.log`, `…055510.log` (I1); `…060321.log` (I2); `docs/P1_integration_package.md` §9, §10 | 9 November 2026 |

## 3. Decision log
Decisions (D) and blueprint diffs (B), taken from the tables in the module docs.
- Summaries are cut short where long. Each module doc has the full text and the reason.
- "Adopted" means the decision as recorded in its module.
- B diffs are listed with the blueprint revision that applied them.

**Missing from this table:**
- **D-M2 decisions:** not recorded. The M2 doc has no decision table, and the monitor output wasn't saved before 30 October 2026.
- **F items:** not recorded. M2 findings F1 onwards are referred to in other docs ("as in M2 F5", `docs/M3_package.md`), but they were never written into a doc and the M2 output wasn't saved.
- **M4 C1 and C2:** listed in `docs/M4_package.md` (C1 is the bus line and boot diagnostics; C2 is ToF re-initialisation, now risk R8).
- **D-P1-1 to D-P1-3:** see `docs/P1_integration_package.md` §2.1.

| ID | Module | Decision or diff (from the module doc) | Outcome |
|---|---|---|---|
| D-M3-01 | M3 | The Core 1 init does not log. The driver returns an `InitStatus` (step, register, expected and actual values, WHO_AM_I, PWR_MGMT_1 after reset), the … | Adopted |
| D-M3-02 | M3 | Bench-only ISR instrumentation behind the compile-time flag `FE_IMU_ISR_INSTRUMENT` (on only when `FE_M3_TEST` is on): raw-interval min and max, the … | Adopted |
| D-M3-03 | M3 | The free heap is sampled by the Core 0 reporter when it sees Phase B start and end, and the Core 1 task's stack high-water mark is read from Core 0 … | Adopted |
| D-M3-04 | M3 | "Steady" means every 10 s window rate within 1 % of the phase mean | Adopted |
| D-M3-05 | M3 | Phase A is split into A1 and A2 (see below). The run is about 12.5 minutes | Adopted |
| D-M3-06 | M3 | Acceptance 7: the accelerometer magnitude band is 0.90 to 1.10 g, and gyro bias beyond 5 °/s is flagged | Adopted |
| D-M3-07 | M3 | CLAUDE.md Core 1 rule and blueprint §4.2 rule 8 reworded: "Never share a user-level mutex, semaphore or queue with Core 0. The FreeRTOS kernel … | Adopted |
| D-M3-08 | M3 | Blueprint §4.2 rule 9 records the `i2c_master_bus_reset()` result: allocation-free; takes the shared periph_spinlock briefly; on the ESP32-S3 the … | Adopted |
| D-M3-09 | M3 | The two remaining MPU-6050 source comments (`board_config.h` bus A comment, `m2_i2c_test.cpp` register comment) are also renamed. The `OK: MPU-6050` … | Adopted |
| D-M4-01 | M4 | 40 Hz: timing budget 20 ms, inter-measurement period 25 ms | Adopted |
| D-M4-02 | M4 | A distance is published as valid only for the library's "range valid" status. Every other status is invalid. The raw and mapped statuses are always … | Adopted |
| D-M4-03 | M4 | Readings below 40 mm are also invalid | Adopted |
| D-M4-04 | M4 | If no data-ready arrives for 3 periods (75 ms), `tof_task` publishes an invalid "no data" record | Adopted |
| D-M4-05 | M4 | P1 standard deviation is compared with 1 % repeatability (DS p.21), and P2 errors with ±20 mm (DS p.22, Table 19) plus 5 mm for the tape. Comparisons … | Adopted |
| D-M4-06 | M4 | `FE_IMU_ISR_INSTRUMENT` is on for M3 or M4. The M3 test builds under either flag and offers `m3_phase_b_start(seconds)` | Adopted |
| D-M4-07 | M4 | Header-only `components/seqlock` (`SeqlockSlot<T>`), used by `tof_task` and the M3 test | Adopted |
| D-M4-09 | M4 | **Long and Medium considered, not adopted; Short mode kept.** Can be reopened through a blueprint diff | Adopted |
| D-M4-10 | M4 | `board::kTofDistanceMode` (default Short), applied in `drv_vl53l1x::init()` through `set_distance_mode()`. Compile-time checks: budget at least 20 ms … | Adopted |
| D-M4-08 | M4 | Inter-measurement period register = `period_ms × (OSC_CALIBRATE_VAL & 0x3FF) × 1.075` (ST `VL53L1X_SetInterMeasurementInMs()`, SparkFun BSD-3 copy), … | Adopted |
| D-M5-01 | M5 | LittleFS from the pinned managed component `joltwallet/littlefs ==1.22.3`; `dependencies.lock` committed, `managed_components/` ignored | Adopted |
| D-M5-02 | M5 | Manifest in `components/hal_fs/idf_component.yml` rather than `src/` | Adopted |
| D-M5-03 | M5 | Read-only mount plus the partition `readonly` flag | Adopted |
| D-M5-04 | M5 | `CONFIG_LITTLEFS_MMAP_PARTITION=y` | Adopted |
| D-M5-05 | M5 | No NVS at all: Wi-Fi NVS off, PHY calibration not stored, `nvs_flash_init()` never called | Adopted |
| D-M5-06 | M5 | `GET /version` (JSON from `esp_app_get_description()`) | Adopted |
| D-M5-07 | M5 | `..` in a path returns 400 | Adopted |
| D-M5-08 | M5 | M3 Phase B can run again on the same tasks (`m3_phase_b_start()` after `m3_phase_b_result()`); the Core 1 task polls an atomic flag between runs | Adopted |
| D-M5-09 | M5 | No "second device refused" line | Adopted |
| D-M5-10 | M5 | Acceptance 6 idle reference: internal free heap with the AP up, the laptop joined, before the load starts | Adopted |
| D-M5-11 | M5 | Footprint compared with the M4 build | Adopted |
| D-M5-12 | M5 | Test file of incompressible deterministic bytes | Adopted |
| B-M5-01 | M5 | §4.9: Add `CONFIG_ESP_WIFI_NVS_ENABLED=n`, `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n`, `CONFIG_LITTLEFS_MMAP_PARTITION=y`, no NVS initialised | Applied, blueprint revision 2.8 |
| B-M5-02 | M5 | §6.2 R4: "Flash operations, including `esp_flash_read()` reads, disable the cache on both cores" with the mitigation: no writes, cache-mapped reads | Applied, blueprint revision 2.8 |
| B-M5-03 | M5 | §4.6: Add `GET /version` | Applied, blueprint revision 2.8 |
| B-M5-04 | M5 | §4.10: Sources in `web/`, `data/` generated by `tools/build_web.py` (git-ignored) | Applied, blueprint revision 2.8 |
| B-M5-05 | M5 | §5 Phase 1, M5 row: Add the IMU-under-Wi-Fi acceptance and no runtime flash writes | Applied, blueprint revision 2.8 |
| B-M5-06 | M5 | §3.4 or §4.6: Partition `storage` flagged `readonly` | Applied, blueprint revision 2.8 |
| B-M5-07 | M5 | §6.2 R5, candidate fixes: "I2C interrupt already in IRAM" (`CONFIG_I2C_ISR_IRAM_SAFE=y` in `sdkconfig.defaults`, … | Applied, blueprint revision 2.8 |
| B-M5-08 | M5 | §6.2 R4: Flash operations through the SPI flash API (writes, erases and `esp_flash_read()` reads) disable the cache on both cores; LittleFS reads go … | Applied, blueprint revision 2.8 |
| D-M6-01 | M6 | One setpoint slot per transport (`comms::setpoint::slot(kUdp/kWs)`); Core 1 reads both and uses the newer by receive time | Adopted |
| D-M6-02 | M6 | CRC-8: polynomial 0x07, init 0x00, no reflection, no final XOR, over bytes 0 to 12 (CRC-8/SMBUS, check value 0xF4 for `"123456789"`) | Adopted |
| D-M6-03 | M6 | Ping and pong: the pong (type 3) echoes seq, roll, pitch, yaw and flags; throttle carries the device turnaround in µs (receive to send, saturating at … | Adopted |
| D-M6-04 | M6 | Clients send a control frame half a period after each ping: 50 Hz each, 100 frames/s per transport (`--no-control` turns them off). Control frames … | Adopted |
| D-M6-05 | M6 | Host unit test with the laptop's g++ (approved): `g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/comms/include components/comms/control_frame.cpp … | Adopted |
| D-M6-06 | M6 | Shared known-answer vectors `components/comms/test_vectors/control_frame_vectors.txt`, used by the firmware (embedded, `SUMMARY CRC` at boot), the … | Adopted |
| D-M6-07 | M6 | Python tools run in a git-ignored `.venv` made with uv: `uv venv .venv && uv pip install --python .venv/bin/python -r tools/requirements-m6.txt` … | Adopted |
| D-M6-08 | M6 | `/ws` is registered by `http_static::start(extra)` ahead of the `/*` wildcard; the M5 "`/ws` → 404" branch is removed | Adopted |
| D-M6-09 | M6 | `platformio.ini` lists the vectors file under `board_build.embed_txtfiles` | Adopted |
| B-M6-01 | M6 | §4.2: One seqlock setpoint slot per transport; Core 1 uses the newer | Applied, blueprint revision 2.9 |
| B-M6-02 | M6 | §4.6 control frame: CRC-8 poly 0x07, init 0x00, no reflection, no final XOR (check 0xF4), shared vectors file | Applied, blueprint revision 2.9 |
| B-M6-03 | M6 | §4.6: Pong echoes seq, roll, pitch, yaw, flags; throttle = device turnaround µs, saturating | Applied, blueprint revision 2.9 |
| B-M6-04 | M6 | §4.6: UDP port 5858 | Applied, blueprint revision 2.9 |
| B-M6-05 | M6 | §4.6: Binary 14-byte frames only; any other frame closes the session; plain GET 400; handler in the httpd task on Core 0 | Applied, blueprint revision 2.9 |
| B-M6-06 | M6 | §4.9: `CONFIG_HTTPD_WS_SUPPORT=y` | Applied, blueprint revision 2.9 |
| B-M6-07 | M6 | §4.6 joystick UI: No screen wake lock over plain http (browsers require a secure context); hidden page stops sending; the user sets the screen timeout | Applied, blueprint revision 2.9 |
| B-M6-08 | M6 | §4.6 Wi-Fi AP: Set `dtim_period = 1` explicitly (now 0 in a zeroed config, IDF default 1) | Applied, blueprint revision 2.9 |
| B-M6-09 | M6 | §5 Phase 1, M6 row: Add: IMU alongside (acceptance 6), setpoint slot (acceptance 7), heap (acceptance 8) | Applied, blueprint revision 2.9 |
| B-M6-10 | M6 | §4.2 setpoint hand-off: The Core 0 writer wraps each slot write in a short Core 0 critical section (interrupts off for the copy, about a microsecond; … | Applied, blueprint revision 2.9 |
| B-M6-11 | M6 | §4.3 Core 1 loop, R5: Put the Core 1 setpoint read (hook, `SeqlockSlot::read`, check word) in IRAM, or measure … | Applied, blueprint revision 2.9 |
| B-M6-12 | M6 | §4.6 ground station: The laptop ground station runs with Wi-Fi power save off and with background scanning sources (location services) off; a scan on … | Applied, blueprint revision 2.9 |
| B-M7a-01 | M7a | §4.9: `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM=y` only. The ISR option is `CONFIG_MCPWM_ISR_CACHE_SAFE` in IDF 5.5.3 and is not needed (no MCPWM interrupts) | Applied, blueprint revision 2.10 |
| B-M7a-02 | M7a | §3.2 hardware cutoff: Force-low through `hal/mcpwm_ll.h` continuous force (immediate update) in IRAM, IDs verified at init; release and force with … | Applied, blueprint revision 2.10 |
| B-M7a-03 | M7a | §3.2 PWM: PLL_F160M / 2 / 2 = 40 MHz, 2000 ticks, prescalers 1 to 256, 16-bit peak; compare updated at counter zero; duty 0 held by the force | Applied, blueprint revision 2.10 |
| B-M7a-04 | M7a | §4.2: `configMAX_PRIORITIES - 2`: the IPC task holds the top priority on each core; same priority as the M3 IMU task | Applied, blueprint revision 2.10 |
| B-M7a-05 | M7a | §4.8 cut triggers: Detected from the last ISR timestamp: a 3-tick wait, then one tick at a time until more than 3000 µs have passed; decision more … | Applied, blueprint revision 2.10 |
| B-M7a-06 | M7a | §4.8 arm conditions: Arm flag low then high in fresh frames after the latest boot, reconnect (stale to fresh, or a transport change) or cut; a … | Applied, blueprint revision 2.10 |
| B-M7a-07 | M7a | §4.8 arm conditions: Thresholds of §1.1: calibration on variation, g0 from the calibration window, motion per 100-sample block, tilt relative to body … | Applied, blueprint revision 2.10 |
| B-M7a-08 | M7a | §4.8 state machine: BOOT: before init, forced low. SAFE: outputs and IMU running, waiting for calibration and a healthy IMU | Applied, blueprint revision 2.10 |
| B-M7a-09 | M7a | §4.2 rule 4 and R5: Flight, safety and hal_mcpwm code in IRAM through component linker fragments. The IDF I2C master transfer stays in flash and logs … | Applied, blueprint revision 2.10 |
| B-M7a-10 | M7a | §4.6 ping and pong: Unchanged; record that the pong carries no device state (the device state is on the serial log); a status frame is a later … | Applied, blueprint revision 2.10 |
| B-M7a-11 | M7a | §5 Phase 1, M7 row: M7a: motor-output layer, state machine, link-loss, kill and missing-IMU cuts (this package). M7b: GPTimer heartbeat watchdog, … | Applied, blueprint revision 2.10 |
| B-M7a-12 | M7a | §4.2 rule 8: Implemented with `portENTER_CRITICAL` and one `portMUX_TYPE` per slot (§1 Part 0). Bench: lock held at most 16.6 µs (seqlock write … | Applied, blueprint revision 2.10 |
| B-M7a-13 | M7a | §4.2 rule 8, seqlock: Reader copies the buffer holding the last complete value even during a write, and retries only if the writer lapped it (§1 Part … | Applied, blueprint revision 2.10 |
| M7b D1 | M7b | GPTimer interrupt allocated on **Core 1** (the flight task calls `wdt::start()`), alarm every **500 µs**, interrupt **level 3** (the highest the IDF … | Adopted |
| M7b D1b | M7b | **Panic force-low:** `-Wl,--wrap=esp_panic_handler`; the wrapper writes both operators' `gen_force` registers to "forced low", then calls the real … | Adopted |
| M7b D2 | M7b | The ISR reads the heartbeat time and the last data-ready timestamp, cuts on a heartbeat older than 5 ms or data-ready missing for more than 3 ms, … | Adopted |
| M7b D3 | M7b | The setpoint critical section is measured with and without it. Removal is recommended if the results are equal, and becomes the default only after … | Adopted |
| M7b D5 | M7b | IMU recovery: `i2c_master_bus_reset()`, then IMU init. The bus teardown and rebuild (it allocates on Core 1) is replaced by a **controlled restart, … | Adopted |
| M7b D6 | M7b | Key `z`: the flight task puts the IMU to sleep, which stops data-ready (the recovery test). \| | Adopted |
| B-M7b-01 | M7b | §4.2 diagram: GPTimer ISR (IRAM), alarm every 500 µs, allocated on Core 1 at level 3: heartbeat > 5 ms or data-ready missing > 3 ms, enforced only … | Applied, blueprint revision 2.11 |
| B-M7b-02 | M7b | §4.8 cut triggers: Decided by the watchdog ISR: data-ready 3.0 to 3.5 ms, heartbeat 5.0 to 5.5 ms (decision after the last pulse or heartbeat). … | Applied, blueprint revision 2.11 |
| B-M7b-03 | M7b | §4.8 cut action: Plus a hardware-path hard latch: set only by the watchdog ISR, cleared only on entering ARMED by the normal arm sequence; while set, … | Applied, blueprint revision 2.11 |
| B-M7b-04 | M7b | §4.8 other layers: Interrupt watchdog 100 ms with the CPU1 tick watched; task watchdog 2 s **with panic** on IDLE0 and IDLE1; every panic forces the … | Applied, blueprint revision 2.11 |
| B-M7b-05 | M7b | §4.9: Add `CONFIG_ESP_INT_WDT_TIMEOUT_MS=100`, `CONFIG_ESP_INT_WDT_CHECK_CPU1=y`, `CONFIG_ESP_TASK_WDT_PANIC=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=2`, … | Applied, blueprint revision 2.11 |
| B-M7b-06 | M7b | §4.2 Core 1: Core 1 runs only flight, IDLE1 and ipc1, plus the data-ready GPIO, GPTimer, I²C0 and system interrupts; "Tmr Svc" pinned to CPU0 (it was … | Applied, blueprint revision 2.11 |
| B-M7b-07 | M7b | §4.2 rule 9, CLAUDE.md key decision: Recovery by `i2c_master_bus_reset()` plus IMU init, in the flight task, SAFE or DISARMED only, 3 attempts per … | Applied, blueprint revision 2.11 |
| B-M7b-08 | M7b | §4.8 state machine: Plus DISARMED → SAFE after an IMU recovery (recalibration), and SAFE or DISARMED → BOOT (final) when the restart limit is reached | Applied, blueprint revision 2.11 |
| B-M7b-09 | M7b | §4.2 rule 4, IDF log exceptions: Also `i2c_master_bus_reset()` logs on failure (recovery path, disarmed only), and `abort()` prints before panicking … | Applied, blueprint revision 2.11 |
| B-M7b-10 | M7b | §5 Phase 1: M7b: safety mechanisms (acceptance 1 to 6, 10, 11, 13, 14); M7c: latency histograms, synthetic load, jitter, R5, soak (7, 8, 9, 12) | Applied, blueprint revision 2.11 |
| B-M7b-11 | M7b | §4.2 rule 8: Removed (`kSetpointCriticalSection = false`): the A/B gave 0 give-ups and 0 torn reads on Core 1 either way, and the write costs 0.2 µs … | Applied, blueprint revision 2.11 |
| B-M7b-12 | M7b | Boot sequence (§4.8 BOOT): Unconditional 9-clock plus STOP before the first transfer, and up to 3 IMU init attempts (bus reset, DEVICE_RESET, 100 ms) | Applied, blueprint revision 2.11 |
| M7c D1 | M7c | The M7c bench is the M7b bench plus `src/m7c_timing.{h,cpp}`. `FE_M7C_TEST` replaces `FE_M7B_TEST`; `FE_M7B_BENCH` builds the shared bench for either … | Adopted |
| M7c D2 | M7c | Histograms as in §1.3 (loop period as the deviation from 1000 µs, 1 µs bins) \| | Adopted |
| M7c D3 | M7c | Condition keys `0` to `4`, `n` and `L` (§1.4) \| | Adopted |
| M7c D4 | M7c | R5 builds through project Kconfig options and per-environment overlays (§1.5); the (d) schedule reads the accelerometer at 250 Hz \| | Adopted |
| M7c D5 | M7c | The watchdog's 10 s line now carries the time of the worst alarm delay; the boot-time delay is investigated, nothing changed without approval \| | Adopted |
| B-M7c-01 | M7c | §4.3 timing budget: The M7c measurements: loop work p99.9 and max with the 150 µs synthetic load under page serving; margin stated from the max | Applied, blueprint revision 2.12 |
| B-M7c-02 | M7c | §4.8 arm conditions: An accelerometer value older than 8 ms is stale: the rest, tilt and accelerometer checks fail (arming refused); not a cut | Applied, blueprint revision 2.12 |
| B-M7c-03 | M7c | §4.9: Bench-only Kconfig options `CONFIG_FE_LATENCY_HIST`, `CONFIG_FE_R5_I2C_IRAM`, `CONFIG_FE_R5_GYRO_ONLY` (`components/board_config/Kconfig`), set … | Applied, blueprint revision 2.12 |
| B-M7c-04 | M7c | §6.2 R5: The R5 table (a) to (e) with the burst average and the loop-work p99.9 and max, the IRAM and RAM cost of each, and the recommendation | Applied, blueprint revision 2.12 |
| B-M7c-05 | M7c | §5 Phase 1, M7c row: Loop jitter defined as \\|wake-to-wake − 1000 µs\\| over consecutive data wake-ups, 1 µs bins; per condition (idle, UDP, … | Applied, blueprint revision 2.12 |
| B-M7c-06 | M7c | §4.10: Add `components/latency/` (header-only histograms), `src/m7c_timing.cpp`, `tools/run_m7b.py`, `tools/analyse_m7b.py` | Applied, blueprint revision 2.12 |
| B-M7c-08 | M7c | §4.8 other layers (B-M7b-04): Bench builds, temporary: IDLE0 not watched (`sdkconfig.defaults.bench`); IDLE1 watched with panic. Flight build … | Applied, blueprint revision 2.12 |
| B-M7c-09 | M7c | §4.6 communications, R3: Recorded limitation: under continuous page serving (20 requests/s) the WebSocket replies queue behind the page sends. In the … | Applied, blueprint revision 2.12 |
| B-M7c-10 | M7c | §4.2 rule 2: The Wi-Fi PHY calibration, at about 0.79 s after start, holds off the Core 1 watchdog interrupt for 513–615 µs once per boot. The board … | Applied, blueprint revision 2.12 |
| B-M7c-11 | M7c | §4.9, R5: Recommended: `CONFIG_FE_R5_I2C_IRAM=y` (the R5 (b) fragment) in the flight build, +7.4 KB internal RAM. Pending approval | Applied, blueprint revision 2.12 |

## 4. Timing budget as measured (1 kHz, 1000 µs frame; blueprint §4.3, revision 2.13)
| Item | Measured | Source |
|---|---|---|
| I²C burst read, 14 B | 450 µs idle (M2, M3); under page serving 605 to 633 µs (M5), 620/775 µs avg/max (M7c baseline); with the transfer in IRAM 479/540 µs (M7c (b), the default) | M3, M5, M7c |
| ISR-to-task wake-up | 7 µs avg, 13 µs max (M3, Wi-Fi off); max 20 to 22 µs under all M7c conditions | M3, M7c |
| Filters, attitude, PID, mixer | Not measured: 150 µs stand-in (synthetic load). [PLACEHOLDER: re-measure when they exist] | M7c |
| MCPWM update, safety step, setpoint read | About 5 µs added to the burst read (M7a, idle AP) | M7a |
| Loop work, worst frame, page serving, 150 µs load | 929 µs with the transfer from flash; 703 µs with it in IRAM | M7c |
| Worst-frame margin (target ≥ 250 µs) | 297 µs | M7c |
| Average margin (target ≥ 350 µs) | 355 µs: loop work average 644.8 µs under the full integration load (page serving, UDP arm cycles, WebSocket pings, ToF, 150 µs load); worst frame 699 µs, margin 301 µs | P1-INT I1 |
| Loop-period jitter p99.9 (target < 100 µs) | 4 to 12 µs per condition; 9 µs with the load | M7c |

## 5. Carried over to Phase 2 and Phase 3
| Item | Where it stands | Owner phase |
|---|---|---|
| O4 power-protection gate before motors run under load (VBAT cutoff, brownout tuning) | VBAT check not implemented; its hook always passes (every bench banner) | Phase 2 (sensing hardware), Phase 3 step 3 (before motors under load) |
| Margin re-measure | Worst frame 297 µs with the stand-in load; re-measure with the real estimator, PID and mixer | Phase 3/4 |
| ToF re-initialisation (R8) | No automatic re-init; required before altitude hold | Before Phase 5 |
| No page serving while ARMED (§4.6, R3) | Rule set in revision 2.13; implemented with the joystick UI; latency re-tested before tether work | UI module, before Phase 4 |
| R5 reserve | 32 KB instruction cache: about 37 µs at the worst frame for 16 KB RAM | When needed |
| Accelerometer calibration (R6) | \|a\| reads 1.068 g at rest (M3) | Before relying on accelerometer correction |
| Brownout watch | Brownouts and crashes under Wi-Fi load on a poor USB cable (M5, M7b, M7c); none since the cable change | Every bench session; Phase 2 power design |
| Core 0 starvation under page serving | Low-priority Core 0 tasks (the bench reporter, `main`) go silent for seconds during continuous page serving; harmless now, relevant to any Core 0 work added later (telemetry, battery task) | Phase 3/4 task design |
| ToF behaviour after an I²C interruption | Recovers by itself after an SDA unplug (I2); R8 still needed for power or XSHUT loss | Before Phase 5 |

## 6. Hardware notes
- **Pull-ups (blueprint §2.2):**
  - 4.7 kΩ external pull-ups to 3V3 on all four I²C lines.
  - The VL53L1X breakout has 10 kΩ on board (combined about 3.2 kΩ on bus B).
  - The IMU breakout's own pull-ups are unconfirmed (open circuit to VCC with power off).
  - The 400 kHz window is about 1 to 7 kΩ.
- **Gate pull-downs:** 10 kΩ from GPIO 4 to 7 to GND (bench stand-in for the shield).
- **Wiring faults found:**
  - Bus B bad solder joints (M4, 30 to 31 October): 37 failed ToF boots, then a dropout to 0.05 Hz.
  - IMU wiring I²C faults (M7a, the 04:01 and 04:08 sessions on 4 November, not counted).
  - IMU probe failures on every boot in the failed M5 run of 31 October.
- **Supply:** the M5 crashes (31 October) and the M7b and M7c brownouts and crashes under page serving stopped after a better USB cable (7 November). Since then: no brownout in 50 minutes of page serving.
- **Not hardware:** the laptop's background Wi-Fi scan caused periodic 1 to 5 s outages (M7c).
- **Parts used:**
  - ESP32-S3-N16R8 board: generic, unmarked;
  - MPU-6500 breakout: generic, unmarked;
  - VL53L1X breakout: generic, unmarked;
  - 4.7 kΩ and 10 kΩ resistors;
  - multimeter;
  - USB cable: generic, unmarked. What fixed the brownouts was a better, shorter cable; no part number.
- **Pin map:** blueprint §3.4: IMU bus A, ToF bus B (SDA 11, SCL 12), XSHUT 8, ToF INT 13 (unused), motors GPIO 4 to 7.

## 7. Inputs Phase 2 needs
- **Pins and buses:** the pin map (blueprint §3.4), two I²C buses at 400 kHz, the pull-up rule (§2.2).
- **Power, measured:**
  - **Logic current: about 0.4 A peak, estimated from the datasheets, not measured.** ESP32-S3 with Wi-Fi transmitting about 340 mA (commonly quoted, verify against the ESP32-S3 datasheet), VL53L1X 40 mA peak (16–18 mA average while ranging, `docs/datasheets/DS_vl53l1x.pdf`), MPU-6500 3.4 mA with 6 axes active (`docs/datasheets/MPU-6500-Datasheet.pdf`). Design the logic supply for at least 600 mA, so the AP2112K (600 mA, verify against its datasheet) stays. Measure when a meter is available (§6). The motor path is a separate budget (O2, Phase 3);
  - the brownout and crash history above, as the argument for bulk capacitance near the module and short supply wiring;
  - [PLACEHOLDER: brownout level chosen by measurement, §4.9].
- **Motor stage:** 20 kHz PWM on GPIO 4 to 7, gates held low by 10 kΩ pull-downs through reset and flashing, emergency force-low path in firmware.
- **VBAT sensing for O4:** [PLACEHOLDER: divider and ADC pin per §3.4].
- **ToF:** XSHUT on GPIO 8 must stay routed (it's the re-init path for R8); the INT pin is optional.
- **Consoles:** UART0 and native USB both log; flash in DIO mode.
- From the integration run: the ToF and the IMU coexist on separate buses with no interference. Keep two separate buses on the PCB, and keep XSHUT routed.
