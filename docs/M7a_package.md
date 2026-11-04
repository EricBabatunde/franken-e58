# Franken-E58 Phase 1, Module 7a: motor-output layer, arming and failsafe state machine

**Status:** M7a passed (bench, 4 November 2026, §9): acceptance 1 to 11 pass, acceptance 7 against the restated bound (B-M7a-05). Closed out: blueprint revision 2.10 applies B-M7a-01 to 13. The bench harness `src/m7a_bench.*` stays: it is the only entry point that runs the flight task until M7b replaces it.

## 0. Safety boundary

- **No motors, no MOSFET stage and no shield are connected during M7a.** The outputs on GPIO 4 to 7 exist only to prove the state machine; a multimeter is the only instrument on them.
- `board_config.h`: bench duty cap `kBenchDutyCapPermille = 200` with `static_assert(<= 300)`; banner `kBenchBanner` = "M7a bench firmware: no motors may be connected", printed three times at boot together with the cap and the VBAT note.
- **Motors under load remain gated by the VBAT cutoff and brownout tuning** (blueprint §6.1 O4, Phase 3 step 3). The VBAT-above-minimum arm condition is **not implemented**: `safety::vbat_ok()` is a marked hook that always passes and must be replaced before motors run under load.
- Nothing on the console can arm or drive the outputs: the keys are 'm' (data-ready interrupt off/on) and 's' (summary) only.

Blueprint sections used: §3.2, §3.4, §4.2, §4.3, §4.5, §4.6, §4.8, Phase 1 M7, risks R4 and R5. M6 is recorded as passed in the blueprint (revision 2.9, Phase 1 table and R3) and in CLAUDE.md; nothing needed adding.

---

## 1. Spec

### Part 0: carried-over items
1. **B-M6-10, setpoint writer critical section.** `comms::link::handle()` wraps each setpoint slot write in `portENTER_CRITICAL(&s_setpoint_mux[t])` / `portEXIT_CRITICAL`, one `portMUX_TYPE` per slot, taken only by that slot's writer task on Core 0 (`udp_rx`, `httpd`); Core 1 never takes it.
   - **Primitive chosen:** `portENTER_CRITICAL` with a dedicated spinlock. On the SMP port it masks interrupts on the calling core up to the FreeRTOS critical level and takes the spinlock (uncontended here), so the writer cannot be preempted or interrupted mid-copy. Interrupt masking alone (`portSET_INTERRUPT_MASK_FROM_ISR`) would also work for a single writer per slot. The spinlock form was chosen because the spec asks for it and IDF's compliance checks apply to it (verify against IDF docs: Critical Sections).
   - Core 1 keeps its 8-try read. On a give-up it uses the last good setpoint of that transport and counts the event.
   - **Bench result (4 November 2026):** 63 give-ups over about 32 000 setpoint writes, 0 torn. The critical section did not remove them: the cause was the reader, which gave up whenever a write was in progress and spent its 8 tries in well under a microsecond, while a write takes about 4 µs. Fixed in the seqlock reader (B-M7a-13, §1 Part 0 item 4).
   - **Measurement:** per transport, cycles of the whole critical section and of the seqlock write alone (the difference is the added Core 0 cost), printed per window and in the 's' summary.
2. **B-M6-08, DTIM.** `wifi_ap_config_t::dtim_period = board::kWifiDtimPeriod` (1) and `beacon_interval = board::kWifiBeaconIntervalTu` (100) set explicitly. IDF 5.5.3 documents `dtim_period` range 1 to 10, default 1, and `beacon_interval` 100 to 60000 TU, default 100 (`esp_wifi_types_generic.h`). Read back with `esp_wifi_get_config()` and printed once ("WIFI (B-M6-08): ...").
3. Blueprint diffs are collected in §6 and not applied.
4. **B-M7a-13, seqlock reader (added after the first bench run).** `SeqlockSlot::read()` no longer gives up while a write is in progress. Write n fills buffer n & 1, so a write in progress never touches the buffer holding the last complete value. The reader copies that buffer (`(s1 / 2) & 1` for an even or odd sequence `s1`) and accepts the copy unless a later write started on that same buffer during the copy (sequence advanced by 3 or more from an even `s1`, by 2 or more from an odd one). A copy taken while a write finishes is at most one write old. The writer's odd store is now a release store, so a reader that sees it also sees the previous write's data. All slots benefit (setpoints, counters, status, windows, events). A give-up now needs the writer to lap the reader, which a 50 Hz writer cannot do; `tools/host_test/test_seqlock.cpp` checks this.
5. **IMU init failure detail (added after the first bench run).** When `drv_mpu6500::init()` fails, the bench prints a second line, `FLIGHT: IMU init failed at <step> | error <name> (0x..) | register 0x.. expected 0x.. read 0x..`. One boot in the 05:38 log failed at WHO_AM_I with the value 0x00 after a reset while armed; the machine stayed in BOOT with the gates forced low, and the next reset recovered.

### Part 1: components
- **`components/hal_mcpwm`** (IDF MCPWM only):
  - One timer, group 0, `MCPWM_TIMER_CLK_SRC_DEFAULT` (PLL_F160M), 40 MHz resolution, up-counting, 2000 ticks per period (20 kHz).
  - **Clock:** 160 MHz / 2 (group prescale) / 2 (timer prescale) = 40 MHz exactly. Both prescalers range 1 to 256 and the peak register is 16 bits (`MCPWM_LL_MAX_*` in IDF 5.5.3 `mcpwm_ll.h`; verify against the TRM MCPWM chapter). The driver logs "adjust timer resolution" if it cannot meet the request exactly.
  - Two operators × two generators, one comparator each, on `board::kMotorGpio` (4, 5, 6, 7).
  - Each generator goes high at counter zero and low on its compare; compare updates take effect at counter zero (`update_cmp_on_tez`); the pad pull-down is enabled.
  - **Start-up order:** every generator is forced low (continuous force) before its actions are set and before the timer is enabled. Each force is read back from the register, and `forced_before_start` is reported.
  - **Force path:** `mcpwm_generator_set_force_level()` is not placed in IRAM in IDF 5.5.3, even with `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM` (its `linker.lf` lists only the compare and period setters), and its error path logs.
    - `force_low_all()` and `release_all()` therefore write the force fields through `hal/mcpwm_ll.h` (`mcpwm_ll_gen_set_continue_force_level`, update method "immediate"), compiled into IRAM functions.
    - The operator and generator IDs are not public: `init()` forces each generator through the public API and confirms that exactly the expected register field changed, or init fails.
    - Both generators of an operator share one force register (read-modify-write), so the four writes run with interrupts masked on the calling core. This means a future ISR force (the GPTimer watchdog) cannot be undone by a release it interrupted.
  - `set_all_permille()` clamps to 0..1000 and calls `mcpwm_comparator_set_compare_value()`. That is IRAM with `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM=y`, now set; it takes no lock and uses ISR-safe checks.
  - **Locks taken by the IRAM paths:** none (local interrupt mask only).
- **`components/safety`** (pure C++, no IDF headers; IRAM via `linker.lf`):
  - **Machine.** BOOT → SAFE → DISARMED → ARMED → (cut) → DISARMED (latched), exactly as §4.8.
    - BOOT: outputs forced low, before init.
    - SAFE: outputs initialised and forced low, IMU running, waiting for gyro calibration and a healthy IMU.
    - DISARMED: ready to arm.
    - ARMED: outputs follow the throttle.
  - **Inputs per iteration:** time, freshest setpoint and its receipt time, transport, throttle, flags, IMU healthy, missing data-ready, gyro calibration done, at rest, accelerometer valid, tilt.
  - **Outputs:** state, outputs allowed, force low, duty, latch, blocking mask and up to two events.
  - **Arm conditions (all required):**
    - link fresh (age below 150 ms);
    - throttle at or below 10 permille for at least 500 ms, timed on fresh frames;
    - an explicit arm edge;
    - gyro calibration complete;
    - at rest;
    - accelerometer valid;
    - tilt within 20 degrees;
    - IMU healthy;
    - KILL clear;
    - VBAT (hook, always passes).
  - **Arm edge:**
    - The arm flag must be seen low in a fresh frame after the latest boot, reconnect or cut, then high.
    - A reconnect is the link turning fresh after being stale, or the freshest setpoint changing transport (UDP has no connections).
    - A high flag on the first frame after boot or a reconnect is refused once as `arm_held_no_edge`.
    - Every attempt consumes the edge: after a refusal the flag must go low and high again.
  - **Cut triggers (ARMED only), in this order:**
    - KILL flag (immediate, in the iteration that first sees the frame);
    - missing data-ready;
    - link silence (age above 150 ms; exactly 150 ms is not a cut).
    - Loop overruns are counted only.
  - **Cut action:** duty to zero and generators forced low, then latched; no automatic re-arm. Re-arm needs throttle low for 500 ms plus a fresh edge, which clears the latch.
  - **Disarm:** clearing the arm flag while ARMED returns to DISARMED with the same force-low, recorded as a disarm, not a cut, and not latched.
  - **Events:** every refusal and every cut records an enumerated reason (`Refusal`, `Cut`), a timestamp, the receipt time of the setpoint, the last data-ready time, throttle and flags. Refusals also carry the mask of every failing reason.
  - **Rest detector.** Gyro calibration, motion and tilt (thresholds in §1.1).
  - **Flash-write guard.** `flash_writes_allowed(State)` is false in ARMED only (unit-tested); the flight task publishes its state every iteration for `flash_writes_allowed()`.
    - Other write paths, all closed today: NVS never initialised (`CONFIG_ESP_WIFI_NVS_ENABLED=n`), PHY calibration storage off, LittleFS mounted read-only on a read-only partition, core dump to none (`CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y`), no OTA.
- **`components/flight`** (Core 1, `configMAX_PRIORITIES - 2`, IRAM via `linker.lf`):
  - **Init on Core 1:** bus A, IMU init and data-ready interrupt exactly as in M3.
  - **Per iteration:**
    1. notification wait;
    2. 14-byte burst read;
    3. scaling and the rest detector;
    4. both setpoint slots read (8 tries, last good on give-up, check word verified), freshest taken;
    5. safety step;
    6. output write.
    - In ARMED, all four outputs are throttle × 200 / 1000 permille, with no mixing.
    - Duty 0 is held by the force rather than a compare value of 0, so nothing relies on the generator's event priority at counter zero.
  - **Missing data-ready:** the wait is 3 ticks. With a 1 ms tick that timeout can expire 2 to 3 ms after the last pulse, so a timeout cuts only once more than 3000 µs have passed since the last ISR timestamp; until then the wait is one tick at a time. The cut therefore falls 3.0 to 4.0 ms after the last pulse.
  - **IMU healthy:** 100 consecutive good reads and data-ready not missing.
  - **Timing:** the M3 window figures (rate, interval sd, burst, wake-up, missed, timeouts, I2C errors) are kept with the same definitions, plus iterations, work time (wake to output written), overruns (over 1000 µs), setpoint reads, give-ups, retries and torn records.
  - **Hand-off to Core 0:** status every 100 iterations, the event ring (16) on each event, and the window every 10 s, all through seqlock slots.
  - **After init:** no logging, no heap, no lock shared with Core 0 (§1.2).
- **`src/m7a_bench.cpp`** (Core 0):
  - **At boot:** the banner, `OUTPUTS` (init report, IDs, forced before start), FS, AP, `WIFI (B-M6-08)`, LINK (HTTP+WS, UDP), FLIGHT (bus, WHO_AM_I, interrupt, stack), and TASKS (core check: `flight` on 1, the rest on 0).
  - **While running:** one `ST` line per second, one `EV` line per event, and the `B wNN` line (M3 layout) plus a `wNN loop` line per window.
  - **Console task:** reads stdin. 'm' asks Core 1 to disable or restore the data-ready interrupt; 's' prints a summary.
  - **Locks on the 'm' path:** `gpio_intr_disable()` is IRAM and lock-free; `gpio_intr_enable()` takes the GPIO driver spinlock and must run on Core 1 (the enable acts on the calling core). The restore cannot happen while ARMED, because a missing IMU cuts first.
- **Also changed:**
  - `hal_gpio` and `drv_mpu6500`: `imu_int_enable()` and `int_enable()`.
  - `board_config.h`: PWM constants, cap, banner, thresholds, `FE_M7A_TEST`, the exclusion check, `FE_IMU_ISR_INSTRUMENT`.
  - `sdkconfig.defaults`: `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM=y`.
  - `main.cpp` and `src/CMakeLists.txt`.

#### 1.1 Thresholds (all in `board_config.h`)
| Item | Value | Reason |
|---|---|---|
| Bench duty cap | 200 permille, `static_assert(<= 300)` | Spec; at most 0.66 V average at full throttle |
| Link fresh / cut | age < 150 ms fresh, > 150 ms cut | §4.8; decision at the 1 kHz loop, so 150 to 151 ms |
| Throttle at minimum | ≤ 10 permille for ≥ 500 ms (fresh frames) | A phone slider may not land on exactly 0 |
| Missing data-ready | > 3000 µs since the last ISR timestamp | §4.8 |
| IMU healthy | 100 consecutive good reads, data-ready present | |
| Gyro calibration | 2000 samples (2 s); each axis sd ≤ 0.5 dps and max − min ≤ 4 dps; \|bias\| ≤ 10 dps (sanity); accelerometer norm sd ≤ 0.01 g; any failure, or a range overflow mid-window, restarts | Noise ~0.1 dps sd, so a 5× margin; tests variation; measured bias ≤ 2.3 dps |
| Reference g0 | mean \|a\| of the calibration window; 0.85 to 1.25 g or "accel bad" | R6: reads 1.068 g; nothing compares with 1 g |
| Moving | per 100-sample block: any axis \|gyro − bias\| > 3 dps, or \|\|a\|/g0 − 1\| > 0.05; at rest = no moving block for 500 ms | |
| Accelerometer valid | \|a\|/g0 within 0.85 to 1.15 | |
| Tilt | cos θ = a_z/\|a\| (board +Z up), θ ≤ 20 degrees (provisional) | Compared as squares: no square root per sample |
| Loop overrun | iteration work > 1000 µs, counted only | |

#### 1.2 Calls reachable from the flight task and the IMU ISR, with the locks they take (acceptance 10)
| Call | Where | Lock |
|---|---|---|
| `ulTaskNotifyTake` (`ulTaskGenericNotifyTake`) | IRAM (FreeRTOS) | kernel spinlock (unavoidable, CLAUDE.md) |
| `esp_timer_get_time` | IRAM | none (reads the systimer counter) |
| `esp_cpu_get_cycle_count` | inline | none |
| `drv_mpu6500::read_burst` → `hal_i2c` → `i2c_master_transmit_receive` | flash | bus A's `bus_lock_mux` and `cmd_semphr`, the bus spinlock, the event queue: all bus A, Core 1 and its I2C ISR only. **IDF logs `ESP_LOGE` on an I2C software or hardware timeout** (`i2c_master.c`); our code never logs |
| `drv_mpu6500::last_notify_us`, `raw_pulses`, `gyro_valid_from_us`, `isr_instrument*` | flash | none (atomics) |
| `drv_mpu6500::int_enable` → `gpio_intr_disable` / `gpio_intr_enable` | IRAM / flash | none / GPIO driver spinlock (bench injection only, never while ARMED) |
| `comms::setpoint::slot`, `SeqlockSlot::read`, `check_word` | flash / inline | none |
| `safety::Machine::step`, `RestDetector::sample`, `publish_state`, `flash_writes_allowed` | IRAM | none (atomics) |
| `hal_mcpwm::force_low_all`, `release_all`, `all_forced_low` | IRAM | none; interrupts masked on Core 1 for four register writes |
| `hal_mcpwm::set_all_permille` → `mcpwm_comparator_set_compare_value` | IRAM | none |
| `SeqlockSlot::write` (status, window, events), `memcpy` | inline / ROM | none |
| IMU ISR: `esp_timer_get_time`, atomics, `vTaskNotifyGiveFromISR`, `portYIELD_FROM_ISR` | IRAM | kernel spinlock in the notify |

**How it was checked:**
- `xtensa-esp32s3-elf-nm -u` on `libflight.a`, `libsafety.a` and `libhal_mcpwm.a` lists no `esp_log*`, `printf`, `malloc`, `calloc`, `free`, `operator new`, `__cxa_guard*` or `__atomic*` symbol. A `fetch_add` and a `double` accumulator were removed for this.
- The two flight-task objects are file-scope (no function-local static guard).
- `nm` on the ELF places every `flight::`, `safety::` and `hal_mcpwm::` function at 0x4037…/0x4038… (IRAM); `memcpy` and `__divsf3` resolve to ROM.

## 2. Runtime flash writes
None added. The guard and the closed paths are listed in §1 (flash-write guard).

## 3. Acceptance criteria
1. Clean build, `tools/verify_sdkconfig.sh` passes, the host unit tests pass.
2. Power-up and reset: gates low before the outputs start (`OUTPUTS: ... all forced low before the timer started: yes`, multimeter 0 V); reboot always comes up DISARMED; a reconnect never arms.
3. Arming refusals, each with its own reason:
   - no link (status line, blocking mask);
   - throttle not minimum;
   - board moving;
   - tilted past the limit;
   - arm flag held high across a reconnect (`arm_held_no_edge`);
   - IMU interrupt disabled (`imu_unhealthy`).
4. Arm and output: arming succeeds only after all conditions. The multimeter on GPIO 4 to 7 shows an average voltage consistent with the duty (about duty × 3.3 V), and 0 V when disarmed.
5. Link-loss trials (at least 20):
   - cut decision age between 150 and 152 ms in every trial;
   - decision to force-low written under 50 µs;
   - multimeter 0 V afterwards;
   - the latch holds;
   - re-arm only with throttle low plus a fresh arm.
6. Kill: gates forced low within 2 ms of the receipt of the frame carrying the flag (measured value reported).
7. Missing data-ready (console 'm'): cut decision more than 3000 µs and at most 4100 µs after the last pulse. The check runs on the 1 ms tick, so the decision lands up to one tick plus the wake-up after 3000 µs (restated after the bench run, which measured 4031 µs against the first bound of 4000 µs; B-M7a-05).
8. A deliberate 200 ms gap in the client (key s) trips the cut. Recorded, since the phone-side Wi-Fi stalls seen in M6 do the same.
9. IMU alongside under UDP and WebSocket traffic: zero missed samples, zero I2C errors, interval sd below 2 µs, jitter below 20 µs, wake max below 50 µs. Burst average and maximum reported against M6.
10. Core 1 evidence: no ESP_LOG, printf or heap use after init (how it was checked), and every call reachable from the flight task and the IMU ISR listed with the locks it takes (§1.2).
11. B-M6-10 and B-M6-08 done and measured: over at least 10 000 frames per transport, give-ups (expect 0), torn records (must be 0) and the added Core 0 cost; DTIM 1 and beacon 100 TU read back.

## 4. Bench procedure
1. **Hardware:**
   - Nothing on GPIO 4 to 7 but the multimeter (DC volts, COM to GND). No motors, MOSFET stage or shield.
   - IMU wired as for M3.
   - Board flat with the IMU's +Z axis up, on a still surface.
2. **Build (laptop):** `pio run`, `tools/verify_sdkconfig.sh`, then both host tests:
   - `g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/safety/include -Icomponents/board_config/include components/safety/safety.cpp components/safety/rest.cpp tools/host_test/test_safety.cpp -o .pio/host/test_safety && .pio/host/test_safety`
   - the seqlock test: `g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -Icomponents/seqlock/include tools/host_test/test_seqlock.cpp -o .pio/host/test_seqlock && .pio/host/test_seqlock`. Expect `seqlock host test: PASS`;
   - the M6 control frame test (unchanged in M7a; rerun because `link.cpp`, which uses the codec, changed): `g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/comms/include components/comms/control_frame.cpp tools/host_test/test_control_frame.cpp -o .pio/host/test_control_frame && .pio/host/test_control_frame components/comms/test_vectors/control_frame_vectors.txt`. Expect `host test: 4043 checks, 0 failed -> PASS`.
3. **Flash (user):** `pio run -t upload`. The LittleFS image is unchanged, so `uploadfs` is not needed.
4. **Monitor on the UART0 socket** (the USB-UART bridge, not VID `303a`). The keys are read from the primary console only; the USB Serial/JTAG console is output only (verify against IDF docs).
5. **Power-up (acceptance 2):**
   - Check `OUTPUTS: PASS ... all forced low before the timer started: yes`, `WIFI (B-M6-08): beacon_interval 100 TU, dtim_period 1 ... PASS`, `FLIGHT: PASS` and `TASKS: PASS`.
   - Multimeter 0 V on each of GPIO 4 to 7.
   - Keep the board still for about 2 s: `EV ... STATE SAFE -> DISARMED`.
   - Press reset once and check that it comes back the same way.
6. **Laptop:** join the AP, Wi-Fi power save off, location services and the Settings Wi-Fi panel closed, `iw event -t | grep -i scan` quiet (M6 §4 step 5).
7. **Refusals (acceptance 3),** with `.venv/bin/python tools/control_client.py --transport udp`:
   - **No link:** before starting the client, the `ST` line shows `arm blocked by: no_link,...`.
   - **Throttle not minimum:** `t`, then `a` → `REFUSED throttle_not_min`; then `d`, `g`.
   - **Moving:** rotate the board slowly by hand and press `a` → `REFUSED moving`; `d`, and put it down.
   - **Tilted:** tilt it past 20 degrees, hold still 1 s, press `a` → `REFUSED tilted`; `d`, level it.
   - **Held across a reconnect:** `t`, `a` (refused, throttle), `g`, then `s`, wait 1 s, `r` → `REFUSED arm_held_no_edge` on the first frame back; `d`.
   - **IMU interrupt disabled:** press `m` in the monitor (`ST ... imu UNHEALTHY ... int OFF`), then `a` → `REFUSED imu_unhealthy`; `d`; `m` again, wait 1 s.
   - **Kill (extra):** `k`, `a` → `REFUSED kill`; `k`, `d`.
   - **Reboot with the arm flag held (acceptance 2):** `a` with the client running, press reset → after boot `REFUSED arm_held_no_edge`, never ARMED; `d`.
   - **After every reset the laptop drops off the AP.** Rejoin it and check `ping 192.168.4.1`, and wait for the `ST` line to show `sp udp age ...` instead of `no_link` before pressing any client key.
8. **Arm and output (acceptance 4).** "Arm" in steps 8 to 11 always means: throttle 0 (`g` until 0), `d`, wait 1 s, `a` → `EV ... ARMED`, and only then raise the throttle with `t`. The board refuses to arm with the throttle above 10 permille.
   - Arm as above.
   - `t` four times (200 permille: duty 40, expect about 0.13 V). Then to 1000 (duty 200, expect about 0.66 V). Note the reading on each of GPIO 4 to 7.
   - `d` → `DISARM`, 0 V.
9. **Kill (acceptance 6):** arm, then `t` four times (throttle 200), `k` → `CUT kill | frame rx -> force-low N us`; multimeter 0 V; `k`, `d`.
10. **Missing data-ready (acceptance 7):** arm, then `t` twice (throttle 100), press `m` in the monitor → `CUT imu_missing | last data-ready -> decision N us`; `m` to restore; `d`.
11. **200 ms gap (acceptance 8):** arm, then `t` twice (throttle 100), `s`, then `r` about 0.3 s later → `CUT link_silence`. Note it; `d`.
12. **Link-loss trials (acceptance 5):** quit the client (`q`).
    - Run `.venv/bin/python tools/control_client.py --transport udp --scenario link_loss_trials 20`.
    - Keep the multimeter on GPIO 4: about 0.07 V while armed at throttle 100 (duty 20), then 0 V after every cut.
    - Repeat with `--transport ws` (20 more).
13. **IMU under traffic and B-M6-10 (acceptance 9 and 11):**
    - Leave 60 s idle.
    - Run `.venv/bin/python tools/latency_test.py --transport udp`, then `--transport ws` (10 000 pings plus 10 000 control frames each, arm flag clear).
    - Press `s` in the monitor.
14. **Analyse:** `python3 tools/analyse_m7a.py logs/device-monitor-<...>.log`. Pass several files in order if the monitor was restarted.
15. **Copy back:**
    - the boot block (OUTPUTS to TASKS);
    - the analyser output;
    - the `s` summary;
    - the multimeter readings per GPIO and throttle;
    - both `latency_test.py` summaries;
    - any warning or error.

## 5. Not testable without hardware
Acceptance 2 to 9 and 11:
- the generator-ID check and the forced-before-start read-back;
- every timing figure (cut ages, force-low times, kill and missing-IMU latencies, burst, wake-up, work time);
- the multimeter readings;
- the critical-section cost and the give-up count;
- the DTIM read-back;
- the console keys on UART0.

Acceptance 1 and 10 were checked here (§8).

## 6. Blueprint diffs (applied in revision 2.10, 4 November 2026)
| ID | Section | Old | New | Reason |
|---|---|---|---|---|
| B-M7a-01 | §4.9 | `CONFIG_MCPWM_ISR_IRAM_SAFE=y`, `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM=y` | `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM=y` only. The ISR option is `CONFIG_MCPWM_ISR_CACHE_SAFE` in IDF 5.5.3 and is not needed (no MCPWM interrupts) | `esp_driver_mcpwm/sdkconfig.rename`; M7a uses no MCPWM ISR |
| B-M7a-02 | §3.2 hardware cutoff | "the MCPWM generators support forcing the output level" | Force-low through `hal/mcpwm_ll.h` continuous force (immediate update) in IRAM, IDs verified at init; release and force with interrupts masked (shared register, read-modify-write) | `mcpwm_generator_set_force_level()` is not IRAM-placed in 5.5.3 and logs on error |
| B-M7a-03 | §3.2 PWM | 40 MHz resolution "verify clock and divider limits" | PLL_F160M / 2 / 2 = 40 MHz, 2000 ticks, prescalers 1 to 256, 16-bit peak; compare updated at counter zero; duty 0 held by the force | IDF 5.5.3 `mcpwm_ll.h`, `mcpwm_com.c` (TRM check still open) |
| B-M7a-04 | §4.2 | `flight_task (highest priority)` | `configMAX_PRIORITIES - 2`: the IPC task holds the top priority on each core; same priority as the M3 IMU task | FreeRTOS SMP in IDF |
| B-M7a-05 | §4.8 cut triggers | Missing IMU data-ready > 3 ms | Detected from the last ISR timestamp: a 3-tick wait, then one tick at a time until more than 3000 µs have passed; decision more than 3000 and at most 4100 µs after the last pulse (one tick plus the wake-up); the M7b GPTimer watchdog can tighten it | 1 ms tick granularity; bench 4031 µs |
| B-M7a-06 | §4.8 arm conditions | "explicit arm edge (never held across boot or reconnect)" | Arm flag low then high in fresh frames after the latest boot, reconnect (stale to fresh, or a transport change) or cut; a refused attempt consumes the edge; throttle minimum ≤ 10 permille; KILL set blocks arming; clearing the arm flag disarms (not a cut, not latched) | Implementation definitions |
| B-M7a-07 | §4.8 arm conditions | "gyro calibration complete with the vehicle at rest", "tilt below a limit" | Thresholds of §1.1: calibration on variation, g0 from the calibration window, motion per 100-sample block, tilt relative to body +Z | R6; bench noise figures |
| B-M7a-08 | §4.8 state machine | BOOT, SAFE not defined | BOOT: before init, forced low. SAFE: outputs and IMU running, waiting for calibration and a healthy IMU | Implementation definitions |
| B-M7a-09 | §4.2 rule 4 and R5 | No `ESP_LOG` in the loop; control code in IRAM or flash | Flight, safety and hal_mcpwm code in IRAM through component linker fragments. The IDF I2C master transfer stays in flash and logs `ESP_LOGE` on an I2C timeout: an IDF exception to the no-log rule, reached only on a bus fault | `i2c_master.c` |
| B-M7a-10 | §4.6 ping and pong | Pong echoes seq, roll, pitch, yaw, flags | Unchanged; record that the pong carries no device state (the device state is on the serial log); a status frame is a later decision | No free field (B-M6-03) |
| B-M7a-11 | §5 Phase 1, M7 row | One M7 module | M7a: motor-output layer, state machine, link-loss, kill and missing-IMU cuts (this package). M7b: GPTimer heartbeat watchdog, full task layout, loop jitter p99.9 < 100 µs | Module split as specified |
| B-M7a-12 | §4.2 rule 8 | (B-M6-10 wording) | Implemented with `portENTER_CRITICAL` and one `portMUX_TYPE` per slot (§1 Part 0). Bench: lock held at most 16.6 µs (seqlock write alone); the outer figures up to 1.7 ms include interrupts serviced just after the section ends. It did not remove the give-ups (B-M7a-13) | B-M6-10 done; first bench run |
| B-M7a-13 | §4.2 rule 8, seqlock | Reader retries while the sequence is odd (a write in progress) | Reader copies the buffer holding the last complete value even during a write, and retries only if the writer lapped it (§1 Part 0 item 4). The B-M6-10 critical section is no longer needed for give-ups; whether to keep it is a later decision | 63 give-ups in the first bench run with 0 torn; the 8 tries span under 1 µs against a write of about 4 µs |

## 7. Spec items that do not fit ESP-IDF 5.5.3 as written
- **"Set-duty and force-low IRAM-safe" through the MCPWM API:** only the compare setter is IRAM-placed. The force path uses the IDF low-level layer instead (B-M7a-02).
- **`CONFIG_MCPWM_ISR_IRAM_SAFE`** (blueprint §4.9): renamed `CONFIG_MCPWM_ISR_CACHE_SAFE`.
- **"Wait timeout of 3 ms":** with a 1 ms tick a 3-tick wait can end after 2 ms. The cut is decided from the ISR timestamp (B-M7a-05).
- **"Highest priority":** the IPC task holds `configMAX_PRIORITIES - 1` on each core.
- **Console keys:** the USB Serial/JTAG secondary console is output only, so the keys work on UART0.
- **Re-enabling the interrupt:** `gpio_intr_enable()` takes the GPIO driver spinlock and acts on the calling core, so the restore runs on Core 1 (bench injection only).
- **"No link" refusal:** an arm request travels in a frame, so without a link there is no attempt to refuse. It shows in the `ST` line's blocking mask instead of as an event.
- **"Prints the pongs' device state":** pongs carry no device state (B-M7a-10). The client has `--ping` for the round trip only.
- **Link-loss scenario:** a frame with the arm flag high and the throttle low is needed for the edge, so the arm step holds throttle 0 for 100 ms before the 2 s at 100 permille.
- **IDF I2C driver logs on timeout** (B-M7a-09).

## 8. Development results (4 November 2026, no hardware)
- **Build:** clean (`pio run -t clean`, then `pio run`), 0 warnings; `sdkconfig.esp32s3` regenerated after the defaults change; `tools/verify_sdkconfig.sh` 26/26.
- **Footprint** (M6 full firmware in brackets):
  - app image 878 080 B (873 440);
  - `.iram0.text` 101 991 B (94 247; +7 744 B for flight, safety and hal_mcpwm in IRAM);
  - `.dram0.data` 24 112 B (25 640);
  - `.dram0.bss` 22 872 B (30 024; the M6 test buffers are gone).
- **Safety host test:** 6 690 298 checks, 0 failed:
  - every transition;
  - each arm condition failing alone (table);
  - every pair and all at once;
  - arm held across boot, reconnect, transport change and a cut;
  - a refused attempt consuming the edge;
  - link loss at 150 ms − 1 µs, exactly 150 ms (no cut) and + 1 µs (cut, age recorded);
  - kill;
  - missing data-ready;
  - latch;
  - no automatic re-arm over 5 s;
  - re-arm with throttle high refused and with throttle low accepted;
  - flash guard;
  - the rest detector (2 s calibration, bias within 0.02 dps, g0 = 1.068 accepted, motion restart, 10 dps bound, 500 ms rest hold, 15 and 25 degree tilt, 0.8 g0, upside down);
  - a randomised run of 3000 episodes (604 389 iterations, 16 % ARMED, 1104 cuts, 1698 refusals) checking at every iteration that outputs are allowed only in ARMED, forced low with zero duty otherwise, never above the cap, and never ARMED with data-ready missing or a silent link.
- **Control frame host test (M6):** 4 043 checks, 0 failed.
- **Seqlock host test** (after the first bench run): one writer and one reader thread for 1.5 s per case, with the new reader and the old one on the same load. Writer back to back: new 5.96 % give-ups (the writer laps the reader), old 98.4 %. Writer pausing 200 ns or 2 µs between writes: new 0 give-ups in 49 and 62 million reads, old 0.83 % and 0.089 %. Torn and backwards reads: 0 in every case. Single-threaded checks pass. On x86 this checks the buffer and sequence logic, not the ESP32-S3 memory ordering.
- **`control_client.py`** against a local recorder:
  - `link_loss_trials 2` over UDP gave 316 frames, mean interval 20.00 ms (max 20.4), two 1.52 s silences, 2 arm edges, throttles 0 and 100.
  - The same over WebSocket gave 317 frames with a normal close (1000).
  - The key handling was exercised directly.
  - The shared vectors check passes at start.
- **`analyse_m7a.py`:** checked on a synthetic log in the firmware's line formats.
- **Core 1 evidence:** §1.2.

## 9. Bench results (4 November 2026)
Logs: `logs/device-monitor-261104-052501.log`, `-053851.log`, `-054419.log` (firmware before the B-M7a-13 reader) and `-064232.log` (final firmware, ELF SHA256 8663ff35...). The 04:01 and 04:08 sessions are not counted: the IMU wiring had I²C faults and one brownout (blueprint R2), and keys were pasted into the monitor.

| # | Result | Verdict |
|---|---|---|
| 1 | Clean build, 0 warnings; `verify_sdkconfig.sh` all OK; host tests pass: safety 6 690 298 checks, control frame 4 043, seqlock PASS | PASS |
| 2 | Every boot: `OUTPUTS: PASS`, all forced low before the timer started; multimeter 0 V. Reset with the arm flag held: `REFUSED arm_held_no_edge`, never ARMED. One boot (after a reset while armed) failed IMU init at WHO_AM_I (0x00) and stayed in BOOT, gates low; the next reset recovered | PASS |
| 3 | Refusal events: throttle_not_min, moving, tilted, arm_held_no_edge, imu_unhealthy, kill; no_link in the status line before the client started | PASS |
| 4 | Multimeter 0.07 V at throttle 100 (duty 20 permille, expected 0.066 V) and 0.65 V at throttle 1000 (duty 200, expected 0.66 V); 0 V disarmed | PASS |
| 5 | 40 link-loss trials (20 UDP, 20 WebSocket) plus the step 11 gap: decision age 150.020 to 150.971 ms, mean 150.50 ms; force-low 1.48 µs after the decision; 0.07 V armed, 0 V after every cut; latch held; re-arm only on a fresh edge | PASS |
| 6 | Kill: frame receipt to decision 457 µs (next IMU sample), to force-low 991 µs; multimeter 0.00 V | PASS |
| 7 | Missing data-ready: last pulse to decision 4031 µs, to force-low 4037 µs | PASS (restated bound) |
| 8 | 200 ms gap (key s): cut at 150.82 ms | PASS |
| 9 | IMU under traffic, all sessions: zero missed samples and I²C errors; interval sd ≤ 0.50 µs; jitter ≤ 13.3 µs; wake-up max 15 µs (UDP) and 17 µs (WebSocket); burst average UDP 456 to 481 µs, WebSocket 514 to 529 µs (M6: 470 to 480 and 514 to 534); loop work max 798.6 µs, 0 overruns | PASS |
| 10 | §1.2 and §8 | PASS |
| 11 | Final firmware: 13 964 UDP and 10 000 WebSocket setpoint writes, 0 give-ups, 0 torn; critical section held at most 16.6 µs (seqlock write alone), 2.3 to 3.2 µs added per write on average (the outer maxima up to 1.7 ms include interrupts serviced just after the section ends). Before B-M7a-13: 63 give-ups in about 32 000 writes, 0 torn. DTIM 1 and beacon 100 TU read back on every boot | PASS |

Latency tests (10 000 pings plus 10 000 control frames each, all PASS, 0 lost):
- earlier firmware (`logs/m6/udp-261104-055508`, `ws-261104-055836`): UDP p95/p99 1.99/2.67 ms, WebSocket 3.89/4.97 ms;
- final firmware (`logs/m6/udp-261104-064831`, `ws-261104-065250`): UDP 1.81/2.31 ms, WebSocket 3.67/5.56 ms.

Findings carried forward:
- **Missing data-ready timing** depends on the 1 ms tick; the M7b GPTimer watchdog is the place to make it exact.
- **The B-M6-10 critical section** no longer prevents give-ups since B-M7a-13; whether to keep it is open (blueprint §4.2 rule 8).
- **IMU init after a reset while armed** failed once; the FLIGHT line now prints the failing step and error, and a retry was not added.
- **Burst average under traffic** did not improve with the flight code in IRAM; the I²C transfer still runs from flash (blueprint R5).

## 10. Files
```
components/hal_mcpwm/   CMakeLists.txt, include/hal_mcpwm.h, hal_mcpwm.cpp
components/safety/      CMakeLists.txt, linker.lf, include/safety.h, safety.cpp, rest.cpp
components/flight/      CMakeLists.txt, linker.lf, include/flight.h, flight.cpp
src/m7a_bench.h, src/m7a_bench.cpp
tools/host_test/test_safety.cpp, tools/host_test/test_seqlock.cpp, tools/control_client.py, tools/analyse_m7a.py
docs/M7a_package.md
```
Also changed:
- `components/board_config/include/board_config.h`;
- `components/comms/link.cpp`, `include/link.h` (critical section, write cost), `wifi_ap.cpp`, `include/wifi_ap.h` (DTIM, beacon, read-back), `CMakeLists.txt`;
- `components/hal_gpio` (`imu_int_enable`);
- `components/drv_mpu6500` (`int_enable`);
- `components/seqlock/include/seqlock.h` (B-M7a-13 reader);
- `sdkconfig.defaults`;
- `src/main.cpp`, `src/CMakeLists.txt`;
- `CLAUDE.md`.
