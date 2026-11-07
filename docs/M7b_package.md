# Franken-E58 Phase 1, Module 7b: GPTimer heartbeat watchdog, hard latch, panic force-low, fault injection and IMU recovery

**Status:** **passed and closed 7 November 2026** (bench results §8b). Blueprint diffs B-M7b-01 to 12 applied in blueprint revision 2.11. M7c (latency histograms, synthetic load, jitter per condition, R5 experiments, 20-minute soak) follows once M7b passes.

## 0. Safety boundary

- **No motors, no MOSFET stage and no shield.** The user fits 10 kΩ pull-downs from GPIO 4 to 7 to GND to imitate the shield's gate pull-downs; a multimeter is the only other instrument on them.
- The bench duty cap stays: `kBenchDutyCapPermille = 200`, `static_assert(<= 300)`.
- The start-up banner stays, renamed for the module: "M7b bench firmware: no motors may be connected", printed three times.
- **Nothing on the console can arm or drive an output.**
  - The keys only inject faults (`h`, `H`, `T`, `X`, `z`, `m`) or print (`s`).
  - None of them is reachable from the network.
- **VBAT:** the check is still a hook that always passes (O4). Motors under load stay gated by the VBAT cutoff and the brownout tuning.

Blueprint sections used: §3.2, §4.2, §4.3, §4.8, §4.9, Phase 1 M7, risks R2, R4 and R5.

---

## 1. Spec

### 1.1 Scope (as split and approved)
**M7b covers:**
- the GPTimer heartbeat watchdog and its hard latch;
- the watchdog configuration;
- the panic force-low wrapper;
- the task layout proof and the lock list;
- the fault keys `h`, `H`, `T`, `X` (plus `z` and the existing `m` and `s`);
- reset reasons and the brownout counter;
- IMU recovery while disarmed, with bounded controlled restarts;
- boot robustness after a reset while armed;
- the critical-section A/B measurement;
- host tests and `tools/analyse_m7b.py`.

**M7b acceptance:** 1 to 6, 10, 11, 13 and 14.

**Moved to M7c:**
- the latency histograms;
- the 150 µs synthetic load;
- jitter per condition;
- the R5 experiment builds;
- `tools/run_m7b.py`;
- the 20-minute soak.

These are acceptance 7, 8, 9 and 12.

### 1.2 Decisions (approved)
| ID | Decision |
|---|---|
| D1 | GPTimer interrupt allocated on **Core 1** (the flight task calls `wdt::start()`), alarm every **500 µs**, interrupt **level 3** (the highest the IDF GPTimer driver accepts: `ESP_INTR_FLAG_LOWMED`). Atomicity of the shared MCPWM force register comes from masking interrupts on Core 1; no lock is shared with Core 0. |
| D1b | **Panic force-low:** `-Wl,--wrap=esp_panic_handler`; the wrapper writes both operators' `gen_force` registers to "forced low", then calls the real handler. **Interrupt watchdog 100 ms.** |
| D2 | The ISR reads the heartbeat time and the last data-ready timestamp, cuts on a heartbeat older than 5 ms or data-ready missing for more than 3 ms, through `hal_mcpwm::hard_cut()`, and sets the hard latch. Distinct cut reasons, and a backup tick check in the flight task at 6 ms. |
| D3 | The setpoint critical section is measured with and without it. Removal is recommended if the results are equal, and becomes the default only after approval. |
| D5 | IMU recovery: `i2c_master_bus_reset()`, then IMU init. The bus teardown and rebuild (it allocates on Core 1) is replaced by a **controlled restart, at most 2 per power-on** (RTC memory); after that the board stays in BOOT with the gates low. |
| D6 | Key `z`: the flight task puts the IMU to sleep, which stops data-ready (the recovery test). |
| Additions | 1. The ISR enforces its thresholds **only while ARMED**; in SAFE and DISARMED it only counts. 2. Key `X`: the flight task aborts. 3. The wrapper is minimal: IRAM, constant register writes, then `__real_esp_panic_handler`. 4. `CONFIG_FREERTOS_USE_TRACE_FACILITY` goes in a bench-only overlay. 5. The D5 restart limit is kept in RTC memory. 6. Acceptance 14 is added. |
| Settings | Task watchdog with panic, 2 s. The FreeRTOS timer task ("Tmr Svc") is pinned to CPU0. |

### 1.3 Thresholds (all in `board_config.h`)
| Item | Value | Reason |
|---|---|---|
| Watchdog period | 500 µs (`kWdtPeriodUs`), 10 MHz timer, 5000 ticks | D1. Decision bounds: 5.0 to 5.5 ms and 3.0 to 3.5 ms |
| Heartbeat stall | > 5000 µs (`kWdtHeartbeatStallUs`) | §4.8. The heartbeat gap in normal running is at most about 3 ms (the 3-tick wait), so there is margin; a `static_assert` guards it |
| Missing data-ready (ISR) | > 3000 µs (`kImuMissingCutUs`) | §4.8 |
| Backup tick check | > 6000 µs (`kImuMissingTickCutUs`), reason `imu_missing_tick` | Fires only if the ISR is dead; above the ISR's 3.5 ms bound, so it never wins the race |
| Watchdog dead | ISR count unchanged for > 2000 µs (4 periods) | Refusal `wd_dead` while DISARMED, cut `wd_dead` while ARMED |
| IMU recovery | data-ready missing > 50 ms while SAFE or DISARMED; 3 attempts per episode, 1 s apart | 50 ms: far from the 3 ms cut, so a transient is not "recovered" |
| Controlled restarts | ≤ 2 per power-on (`kRestartLimit`) | Addition 5 |
| Boot retry | `kImuBootRetry = false` for the first session; then 3 attempts, 100 ms wait | §1.7: approved and on since 7 November 2026 |
| Fault stalls | `h` 20 ms, `H` 1 s with interrupts masked, `T` 10 s | Spec |

### 1.4 Watchdog ISR and the hard latch
- **Path:**
  1. `hal_gptimer` (a new HAL, so `wdt` never calls IDF directly) calls `wdt::isr()` in IRAM on every alarm.
  2. The ISR increments its count and reads `safety::published_state()`.
  3. Only if the state is ARMED **and** the latch is clear, it checks the heartbeat age, then the data-ready age.
  4. On a violation it calls `hal_mcpwm::hard_cut(reason)`, which masks interrupts, forces all four generators low through the existing IRAM path, sets the latch and unmasks.
  5. The ISR then records the trip: reason, decision time, reference time, cycles to the force.
  6. While the latch is set it makes no new decision.
- **Who sets and clears the latch:**
  - **Set:** only `hal_mcpwm::hard_cut()`, called only from `wdt::isr()` (`components/wdt/wdt.cpp`).
  - **Cleared:** only `hal_mcpwm::clear_hard_cut()`, called only in `flight::task()` (`components/flight/flight.cpp`), in the iteration where `safety::Machine::step()` emits the transition into ARMED (the normal arm sequence: throttle low plus a fresh arm edge), before the outputs are released.
- **While the latch is set** (`output_gate.h`, under interrupt masking):
  - `release_all()` refuses and returns false;
  - `set_all_permille()` writes 0.
  - So a stalled flight task that resumes in the middle of an output write cannot enable an output.
  - The flight task's fault stall sits exactly there, between the decision and the write, so `h` exercises this case.
- **What the flight task does next:**
  - It feeds the latch into `Inputs::hard_cut`; the machine cuts with `wd_heartbeat` or `wd_imu_missing`, ahead of KILL.
  - The event carries the ISR's own record.
- **Core 1 placement cannot catch** anything that masks interrupt levels 1 to 3 on Core 1:
  - a hang with interrupts masked (`H`), or inside a critical section;
  - spinning on a spinlock that Core 0 holds;
  - a hang in a higher-level ISR.
- **The interrupt watchdog covers those:**
  - Each core routes it to its own interrupt 24, which is level 4 on the ESP32-S3 (`int_wdt.c`, `ETS_T1_WDT_INUM`; verify against the TRM).
  - The CPU1 tick feeds it (`CONFIG_ESP_INT_WDT_CHECK_CPU1=y`).
  - When the tick stops for 100 ms, it panics, and the wrapper forces low first.

### 1.5 Watchdog configuration and the worst case from hang to gates low
| Watchdog | Setting (IDF 5.5.3 Kconfig) | Watches | Action |
|---|---|---|---|
| GPTimer heartbeat watchdog (ours) | 500 µs, Core 1, level 3 | flight task heartbeat; data-ready; ARMED only | force low, latch: 5.0 to 5.5 ms or 3.0 to 3.5 ms |
| Interrupt watchdog | `CONFIG_ESP_INT_WDT=y`, `CONFIG_ESP_INT_WDT_TIMEOUT_MS=100` (was 300), `CONFIG_ESP_INT_WDT_CHECK_CPU1=y` | the FreeRTOS tick on each core | panic: wrapper force-low, backtrace, reboot (`INT_WDT`) |
| Task watchdog | `CONFIG_ESP_TASK_WDT_INIT=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=2` (was 5), `CONFIG_ESP_TASK_WDT_PANIC=y` (was n: a timeout only printed), `CHECK_IDLE_TASK_CPU0/CPU1=y` | IDLE0 and IDLE1 being starved | panic: same path (`TASK_WDT`) |
| Panic handler | `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y` | | prints, then reboots; the wrapper runs first |
| Brownout | `CONFIG_ESP_BROWNOUT_DET=y` (level 7) | the 3.3 V rail | reset; the pull-downs hold the gates low; counted in RTC memory |

**Worst case, interrupts-disabled hang on Core 1 to gates low:**
- **Now:** the interrupt watchdog timeout, 100 ms (≤ 100 ms after the last tick on Core 1), plus the panic entry and two register writes (microseconds).
- **Before D1b:** 300 ms plus the print time, until the reset.
- **Measured on the bench:**
  - The `HANG` line at the next boot gives hang to reset on the RTC clock. That is an upper bound, since it includes the backtrace print.
  - The time to the panic itself equals the interrupt watchdog timeout.
- **Bench (§8b):** `H` reset 1.40 to 1.44 s after the hang and `X` 1.21 s, with the same print and reboot, so the time to the panic (gates low) is about 0.2 s, not 0.1 s. Not measured directly.
- **Proposal:** keep 100 ms. Lowering it further trades margin against Wi-Fi driver interrupt latencies, for little gain while the GPTimer watchdog covers the common cases.

### 1.6 Calls reachable from the flight task and its interrupt handlers, with their locks
The M7a §1.2 table still applies. M7b adds:

| Call | Where | Lock |
|---|---|---|
| `wdt::isr` (GPTimer ISR body), `wdt::heartbeat`, `isr_count`, `last_trip`, `take_window` | IRAM | none (atomics, same core) |
| IDF `gptimer_default_isr` (dispatcher) | IRAM (`CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y`) | **timer group spinlock** (`portENTER_CRITICAL_ISR`), uncontended: the only GPTimer in the firmware, used only on Core 1 |
| `hal_mcpwm::hard_cut`, `clear_hard_cut`, `hard_latched`, `release_all`, `set_all_permille`, `force_low_all` | IRAM (`hal_mcpwm/linker.lf`) | none; interrupts masked on Core 1 for the register writes |
| `drv_mpu6500::last_notify_us` | IRAM (moved from flash: the watchdog ISR reads it) | none |
| `safety::published_state` | IRAM | none |
| `__wrap_esp_panic_handler` | IRAM | none: two constant stores, then the real handler (in flash: `CONFIG_ESP_PANIC_HANDLER_IRAM` not set) |
| Recovery only (SAFE or DISARMED): `hal_i2c::bus_reset` → `i2c_master_bus_reset` | flash | the shared `periph_spinlock` briefly (M3); **logs `ESP_LOGE` on failure** (`ESP_RETURN_ON_ERROR`): an IDF exception like the I²C timeout log, reachable only while disarmed |
| Recovery only: `drv_mpu6500::init` (re-run, no allocation: the device exists) | flash | bus A's locks (M7a), `vTaskDelay` (100 ms) |
| Bench keys only: `drv_mpu6500::sleep`, `std::abort` (`X`) | flash | `abort()` prints "abort() was called" before the panic: deliberate |

The kernel spinlock behind task notifications is already known (CLAUDE.md).

### 1.7 Boot robustness (instrument first, then the fix after approval)
- **Hypothesis for the M7a WHO_AM_I 0x00 boot:**
  - A reset in the middle of a burst read can leave the MPU-6500 mid-byte, driving a 1 bit.
  - SDA then reads high, so the old line check ("SDA low only") does nothing.
  - The first transfer then fails.
- **Session 1 (`kImuBootRetry = false`):** every boot prints each init attempt with its step and error code, and the line check result. The 20 resets while armed show how often it happens and why.
- **The fix (`kImuBootRetry = true`, applied after approval):**
  1. Always clock SCL 9 times and send a STOP before creating the bus (`LineAction::kClocked`).
  2. On an init failure, run `i2c_master_bus_reset()`, a best-effort DEVICE_RESET, a 100 ms wait, and the full sequence again.
  3. At most 3 attempts, then BOOT, with the reason printed.
- **Why it can't enable outputs:** `hal_mcpwm::init()` forces every generator low before the flight task starts, and the machine cannot leave BOOT until the IMU, its interrupt and the watchdog are all up.

### 1.8 Bench build
- **Overlay:** `[env:esp32s3_bench]` in `platformio.ini` applies `sdkconfig.defaults.bench` after `sdkconfig.defaults`:

  ```
  board_build.cmake_extra_args = -DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.bench
  ```

  Its generated config is `sdkconfig.esp32s3_bench`. The overlay holds only `CONFIG_FREERTOS_USE_TRACE_FACILITY=y`.
- **Without the overlay:** the default environment builds the same code, and its task table falls back to known task names.
- **Flash with:** `pio run -e esp32s3_bench -t upload`.
- **Verify with:** `tools/verify_sdkconfig.sh sdkconfig.esp32s3_bench sdkconfig.defaults sdkconfig.defaults.bench`.
- **The overlay's age isn't tracked:** PlatformIO only watches `sdkconfig.defaults` for changes, so after editing the overlay, delete `sdkconfig.esp32s3_bench` before rebuilding.

### 1.9 Critical-section A/B
`board::kSetpointCriticalSection` (default `true`) gates `portENTER_CRITICAL` around the Core 0 setpoint write in `link.cpp`. The B build sets it to `false`. The `s` summary prints "critical section ON" or "OFF" with the write costs; the status line shows the give-ups and torn reads.

## 2. Runtime flash writes
None added. The RTC record (32 B, `RTC_NOINIT_ATTR`) is RAM.

## 3. Acceptance criteria (M7b)
1. Clean build of both environments, `tools/verify_sdkconfig.sh` passes for both, and the host unit tests pass (safety, output gate, seqlock, control frame).
2. **Heartbeat stall (`h`, at least 20 trials while ARMED):**
   - cut `wd_heartbeat` with the decision more than 5000 µs and at most 5000 + 500 µs (plus the measured ISR entry latency) after the last heartbeat;
   - decision to force-low under 50 µs;
   - GPIO 4 to 7 read 0 V;
   - the latch holds when the stalled task resumes;
   - re-arm works only with throttle low plus a fresh arm edge.
   - `h` while DISARMED: no cut (addition 1).
3. **Missing data-ready (`m`, at least 20 trials while ARMED):** cut `wd_imu_missing` with the decision more than 3000 µs and at most 3500 µs (plus the entry latency) after the last pulse. Compare with 4031 µs in M7a.
4. **Interrupt-disabled hang (`H`), task watchdog (`T`) and abort (`X`):**
   - a reset happens within the configured time, with reset reason INT_WDT, TASK_WDT or PANIC respectively;
   - 0 V on GPIO 4 to 7 with the pull-downs fitted, also while the backtrace prints;
   - the backtrace prints;
   - the reboot comes up DISARMED;
   - the time from hang to reset is reported (`HANG` line), with a recommendation (§1.5).
5. **Reset while armed, at least 20 times:**
   - each boot ends DISARMED with the IMU initialised, or in BOOT with the gates low and a reported reason;
   - failures are counted;
   - after the retry fix (session 2), none stay stuck.
6. **Task layout:**
   - the task table is printed;
   - Core 1 runs only the flight task, IDLE1, ipc1 and interrupt handlers (`esp_intr_dump`);
   - the lock list is delivered (§1.6).
10. **IMU recovery while DISARMED (`z`):**
    - a recovery attempt within about 50 ms;
    - back to DISARMED after the 2 s calibration, with the time stated;
    - no attempt while ARMED: `z` while ARMED is cut by the watchdog first, and recovery follows in DISARMED.
11. **Critical-section decision:** recorded with give-ups and torn reads over at least 10 000 frames per transport, with and without it.
13. **Core 1 evidence:** no ESP_LOG, printf or heap after init in the flight task and the watchdog ISR, plus how it was checked (§8).
14. **10 minutes ARMED under UDP and WebSocket traffic with the client alive:**
    - zero watchdog cuts and zero resets;
    - if the interrupt watchdog, the task watchdog or the 500 µs ISR trips under load, it is reported before any value changes.

## 4. Bench procedure
**Session 1 runs with `kImuBootRetry = false`.** Steps 1 to 9 come first, then step 10 (the H, T and X resets), then step 11 (the resets while armed).

1. **Hardware:**
   - 10 kΩ from each of GPIO 4, 5, 6 and 7 to GND, and the multimeter (DC volts, COM to GND).
   - No motors, MOSFET stage or shield.
   - IMU wired as in M7a; board flat and still.
2. **Build (laptop):**
   - `pio run` (both environments), then:
     - `tools/verify_sdkconfig.sh`
     - `tools/verify_sdkconfig.sh sdkconfig.esp32s3_bench sdkconfig.defaults sdkconfig.defaults.bench`
   - The four host tests:
     - `g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/safety/include -Icomponents/board_config/include components/safety/safety.cpp components/safety/rest.cpp tools/host_test/test_safety.cpp -o .pio/host/test_safety && .pio/host/test_safety`
     - `g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/hal_mcpwm/include tools/host_test/test_output_gate.cpp -o .pio/host/test_output_gate && .pio/host/test_output_gate`
     - `g++ -std=c++17 -O2 -pthread -Wall -Wextra -Werror -Icomponents/seqlock/include tools/host_test/test_seqlock.cpp -o .pio/host/test_seqlock && .pio/host/test_seqlock`
     - `g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/comms/include components/comms/control_frame.cpp tools/host_test/test_control_frame.cpp -o .pio/host/test_control_frame && .pio/host/test_control_frame components/comms/test_vectors/control_frame_vectors.txt`
3. **Flash (user):** `pio run -e esp32s3_bench -t upload` (the bench environment). `uploadfs` is not needed.
4. **Monitor on the UART0 socket.** Type only the monitor keys there, one at a time, and never paste.
5. **Boot checks (acceptance 1 and 6):**
   - `RESET: reason POWERON`.
   - `CONFIG: interrupt watchdog on 100 ms (CPU1 tick watched) | task watchdog 2 s, panic on, ... | trace facility on (bench overlay) | setpoint critical section ON | IMU boot retry OFF`.
   - `OUTPUTS: PASS ... panic force-low ready`.
   - `FLIGHT: PASS`, and `WDT: PASS ... level 3 on core 1`.
   - The `TASK` lines and `TASKS: PASS`.
   - The `INTERRUPTS` dump: the GPIO, GPTimer and I²C0 interrupts on CPU 1.
   - 0 V on each of GPIO 4 to 7.
6. **Laptop:**
   - Join the AP and check `ping 192.168.4.1`.
   - Start `.venv/bin/python tools/control_client.py --transport udp`.
   - Check that `ST` shows `sp udp age ...`.
   - After every reset, rejoin and check again before pressing any key.
7. **Heartbeat stall (acceptance 2), 20 trials.** Each trial:
   1. Throttle 0, `d`, wait 1 s, `a` → `EV ... ARMED`.
   2. `t` twice: throttle 100, about 0.07 V.
   3. Press `h` in the monitor → `CUT wd_heartbeat | last heartbeat -> watchdog decision N us | force-low written N us after the decision`.
   4. 0 V on the meter.
   5. `ST ... DISARMED (latched) ... hard latch 1`, still not armed with the arm flag high.
   6. In the client: `g` twice, `d`.

   **Once, the re-arm check:**
   1. `t` four times, `d`, `a` → `REFUSED throttle_not_min`, with `hard latch 1` still showing.
   2. `g` to 0, `d`, wait 1 s, `a` → ARMED, `hard latch 0`.
   3. `d`.

   **Once, while DISARMED:** `h` → no cut.
8. **Missing data-ready (acceptance 3), 20 trials.** Each trial:
   1. Arm, then `t` twice.
   2. `m` in the monitor → `CUT wd_imu_missing | last data-ready -> watchdog decision N us`.
   3. `m` again to restore.
   4. `d`.
9. **IMU recovery (acceptance 10):**
   - **While DISARMED:** `z` in the monitor. Expect:
     - `REC #n ... attempt 1/3 state DISARMED ... IMU init OK ... N ms`;
     - `EV ... STATE DISARMED -> SAFE`;
     - about 2 s later, `STATE SAFE -> DISARMED`.
     - Note the times.
   - **While ARMED:** arm, `t` twice, `z`. Expect:
     - `CUT wd_imu_missing` first;
     - then a `REC` line with `state DISARMED`, never ARMED;
     - then `d`.
   - **Optional, restart path:** while DISARMED, pull the IMU SDA jumper. Expect:
     - three `REC ... FAILED` lines, 1 s apart;
     - `RESTART: ... controlled restart 1/2`;
     - after the reboot: `RESET: reason SW (controlled restart ...)` and `FLIGHT: FAIL`, staying in BOOT with 0 V.
     - Refit the jumper and power-cycle.
10. **Hangs (acceptance 4), 3 times each.** Arm at throttle 100 first (about 0.07 V), then press the key:
    - **`H`:**
      - expect `Guru Meditation Error: ... Interrupt wdt timeout on CPU1`, a backtrace, and a reboot;
      - next boot: `RESET: reason INT_WDT`, `HANG: key H | hang -> reset N ms`, then DISARMED.
    - **`T`:**
      - expect `CUT wd_heartbeat` at about 5 ms;
      - after about 2 s, `Task watchdog got triggered ... IDLE1`, a backtrace, and a reboot;
      - next boot: `RESET: reason TASK_WDT`, then DISARMED.
    - **`X`:**
      - expect `abort() was called ... core 1`, a backtrace, and a reboot;
      - next boot: `RESET: reason PANIC`, then DISARMED.
      - **Watch the meter while the backtrace prints:** it must already read 0 V. The wrapper forces low before the print.
    - Rejoin the laptop after each reset.
11. **Reset while armed (acceptance 5), 20 times.** Each time:
    1. Arm at throttle 100.
    2. Press reset.
    3. Rejoin the laptop.
    4. Note `FLIGHT: PASS` or the `IMU init attempt ... failed` line.

    **Session 2 (after the retry is approved):** set `kImuBootRetry = true`, rebuild, flash, and repeat the 20 resets.
12. **Critical section A/B (acceptance 11):**
    1. Quit the client.
    2. Run `.venv/bin/python tools/latency_test.py --transport udp`, then `--transport ws`. That's 10 000 control frames each, with the arm flag clear.
    3. Press `s`.
    4. Set `kSetpointCriticalSection = false`, rebuild, flash, and repeat steps 2 and 3.
13. **Armed under traffic (acceptance 14):**
    1. Arm over UDP at throttle 100 with the client.
    2. In a second terminal: `.venv/bin/python tools/latency_test.py --transport ws --no-control --count 30000` (10 minutes of WebSocket pings).
    3. Keep ARMED for at least 10 minutes. Expect no `CUT` and no reset.
    4. Then `d`, and `s`.
    5. If anything trips, stop and report it before changing any value.
14. **Analyse:** `python3 tools/analyse_m7b.py logs/device-monitor-<...>.log ...`, with all the session's logs in order.
15. **Copy back:**
    - the boot block, including the task table and interrupt dump;
    - the analyser output;
    - the `s` summaries;
    - the multimeter readings: during `X` backtraces, after each cut and after each reset;
    - both latency-test summaries;
    - any warning or error.

## 5. Not testable without hardware
- **Everything in acceptance 2 to 6, 10, 11 and 14:**
  - the watchdog's decision ages and its ISR cost;
  - the panic paths, and the multimeter during a backtrace;
  - the reset reasons and the `HANG` timings;
  - the boot-failure rate after resets while armed;
  - the task table and the interrupt allocation;
  - recovery timing;
  - give-ups with and without the critical section;
  - the 10-minute armed run.
- **The restart limit itself (2 per power-on).** It needs an IMU that fails at run time but initialises at boot. On this board the reset button reports POWERON (seen in M7a), which clears the RTC counters. The limit is Core 0 logic: read in review, not host-tested.
- **The assumption that `esp_timer` counts from the chip reset,** used for the hang-to-reset figure (verify against IDF docs). The `HANG` line also prints the raw values.

## 6. Blueprint diffs (applied in revision 2.11, 7 November 2026)
| ID | Section | Old | New | Reason |
|---|---|---|---|---|
| B-M7b-01 | §4.2 diagram | `GPTimer ISR (IRAM, 5 ms): checks flight_task heartbeat, forces gates low on stall` | GPTimer ISR (IRAM), alarm every 500 µs, allocated on Core 1 at level 3: heartbeat > 5 ms or data-ready missing > 3 ms, enforced only while ARMED; forces gates low and sets the hard latch | D1, addition 1 |
| B-M7b-02 | §4.8 cut triggers | Missing IMU data-ready > 3 ms (tick check, ≤ 4100 µs); `flight_task` heartbeat stall > 5 ms | Decided by the watchdog ISR: data-ready 3.0 to 3.5 ms, heartbeat 5.0 to 5.5 ms (decision after the last pulse or heartbeat). Backup tick check in the flight task at 6 ms. Watchdog dead (no alarm for 2 ms) is a cut while ARMED and a refusal otherwise. Distinct reasons: `wd_heartbeat`, `wd_imu_missing`, `imu_missing_tick`, `wd_dead` | D2 |
| B-M7b-03 | §4.8 cut action | duty to zero and generators forced low, latched | Plus a hardware-path hard latch: set only by the watchdog ISR, cleared only on entering ARMED by the normal arm sequence; while set, release and duty writes are refused under interrupt masking (`output_gate.h`) | D2 |
| B-M7b-04 | §4.8 other layers | task watchdog, brownout, pull-downs | Interrupt watchdog 100 ms with the CPU1 tick watched; task watchdog 2 s **with panic** on IDLE0 and IDLE1; every panic forces the gates low first (`--wrap=esp_panic_handler`); worst case from an interrupts-masked hang to gates low about 100 ms | D1b, settings |
| B-M7b-05 | §4.9 | (no watchdog entries; `CONFIG_MCPWM_...` only) | Add `CONFIG_ESP_INT_WDT_TIMEOUT_MS=100`, `CONFIG_ESP_INT_WDT_CHECK_CPU1=y`, `CONFIG_ESP_TASK_WDT_PANIC=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=2`, `CHECK_IDLE_TASK_CPU0/1=y`, `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`, `CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y`, `CONFIG_GPTIMER_ISR_CACHE_SAFE=y` (approved 7 November 2026: the watchdog interrupt was up to 902 µs late while DISARMED without it, 123 µs at most while ARMED), `CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0=y`; the bench-only overlay with `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` | Settings, addition 4 |
| B-M7b-06 | §4.2 Core 1 | Core 1 runs flight_task | Core 1 runs only flight, IDLE1 and ipc1, plus the data-ready GPIO, GPTimer, I²C0 and system interrupts; "Tmr Svc" pinned to CPU0 (it was unpinned in IDF 5.5.3's defaults) | Task layout finding |
| B-M7b-07 | §4.2 rule 9, CLAUDE.md key decision | Recovery by teardown and rebuild while DISARMED (bench exception) | Recovery by `i2c_master_bus_reset()` plus IMU init, in the flight task, SAFE or DISARMED only, 3 attempts per episode; then a controlled restart (≤ 2 per power-on, RTC memory); then BOOT, final. No rebuild in flight firmware | D5, addition 5 (rebuild allocates on Core 1) |
| B-M7b-08 | §4.8 state machine | BOOT → SAFE → DISARMED → ARMED | Plus DISARMED → SAFE after an IMU recovery (recalibration), and SAFE or DISARMED → BOOT (final) when the restart limit is reached | Recovery |
| B-M7b-09 | §4.2 rule 4, IDF log exceptions | I²C master timeout log only | Also `i2c_master_bus_reset()` logs on failure (recovery path, disarmed only), and `abort()` prints before panicking (bench key `X` only) | §1.6 |
| B-M7b-10 | §5 Phase 1 | M7b: task layout, core pinning, GPTimer watchdog | M7b: safety mechanisms (acceptance 1 to 6, 10, 11, 13, 14); M7c: latency histograms, synthetic load, jitter, R5, soak (7, 8, 9, 12) | Approved split |
| B-M7b-11 | §4.2 rule 8 | Critical section kept, open | Removed (`kSetpointCriticalSection = false`): the A/B gave 0 give-ups and 0 torn reads on Core 1 either way, and the write costs 0.2 µs instead of 3 to 14 µs added on Core 0 | Approved 7 November 2026 |
| B-M7b-12 | Boot sequence (§4.8 BOOT) | — | Unconditional 9-clock plus STOP before the first transfer, and up to 3 IMU init attempts (bus reset, DEVICE_RESET, 100 ms) | Approved 7 November 2026 (`kImuBootRetry = true`); session 2 pending |

## 7. Spec items that do not fit ESP-IDF 5.5.3 as written
1. **Task watchdog panic is off by default** (`CONFIG_ESP_TASK_WDT_PANIC=n`): a timeout only printed, so `T` would never have reset. It is now set.
2. **"Tmr Svc" has no affinity by default** (`CONFIG_FREERTOS_TIMER_TASK_NO_AFFINITY=y`), so it could run on Core 1. It is now pinned to CPU0.
3. **The IDF GPTimer driver accepts interrupt levels 1 to 3 only,** and its default ISR takes the timer group spinlock to clear the interrupt and re-arm the alarm. That's uncontended here and listed in §1.6.
4. **`esp_rtc_get_time_us()` takes a spinlock** (`s_esp_rtc_time_lock`). The hang timestamp is therefore taken on Core 0 when the key is handled (at most one loop period before the stall), never on Core 1.
5. **The spec's fallback, bus teardown and rebuild,** allocates inside the IDF driver on Core 1, against CLAUDE.md. It is replaced by the controlled restart (D5).
6. **`i2c_master_bus_reset()` logs on failure** (`ESP_RETURN_ON_ERROR`).
7. **The panic handler runs from flash** (`CONFIG_ESP_PANIC_HANDLER_IRAM` not set). The wrapper is in IRAM and acts before any flash code.
8. **This board's reset button reports POWERON,** which clears RTC memory. The brownout and restart counters therefore count "since the last reset button or power-on".
9. **The interrupt watchdog interrupt is level 4** (interrupt 24 on each core), so the `H` mask (levels 1 to 3) does not block it (verify against the TRM).

## 8. Development results (5 November 2026, no hardware)
- **Build:**
  - Clean, `pio run -t clean` then `pio run`, both environments: 0 warnings.
  - `sdkconfig.esp32s3` deleted and regenerated after the defaults change.
  - `verify_sdkconfig.sh`: `sdkconfig.esp32s3` 37/37 OK; `sdkconfig.esp32s3_bench` 38/38 OK.
  - The trace facility is on only in the bench config.
- **Footprint, default environment** (M7a in brackets):

  | Section | M7b | M7a |
  |---|---|---|
  | App image | 893 632 B | 878 080 |
  | `.iram0.text` | 105 883 B | 101 991 |
  | `.dram0.data` | 24 588 B | 24 112 |
  | `.dram0.bss` | 25 344 B | 22 872 |
  | `.rtc_noinit` | 32 B | 0 |

  Bench environment: app 895 136 B, `.iram0.text` 107 399 B.
- **Panic wrapper, checked in the linked ELF:**
  - IDF's `panic_handler()` (port/panic_handler.c:266) now references `__wrap_esp_panic_handler` (0x40375a74, IRAM).
  - The wrapper disassembles to: a `s_panic_ready` check, two `s32i` stores of the captured constants to MCPWM0 + 76 and + 132 (operators 0 and 1 `gen_force`), each with `memw`, then `callx8 esp_panic_handler`.
  - So the force comes before any print.
- **IRAM placement:** every new Core 1 symbol is in IRAM (0x4037…): `wdt::isr`, `hal_gptimer::on_alarm`, `hal_mcpwm::hard_cut`, `clear_hard_cut`, the gate templates, `drv_mpu6500::last_notify_us`, `safety::published_state`, `wdt::heartbeat`.
- **Core 1 evidence (acceptance 13):** `nm -u` on libflight, libsafety, libhal_mcpwm, libwdt and libhal_gptimer shows no `esp_log`, printf, puts, malloc, calloc, `operator new`, guard or `__atomic`/`__sync` calls. The one exception is `abort` in libflight, the deliberate `X` path.
- **Safety host test:** 7 455 485 checks, 0 failed (M7a: 6 690 298). New:
  - a watchdog cut while ARMED;
  - the stalled task resuming with arm held for 5 s (no re-arm, latch kept);
  - re-arm refused with throttle high and accepted with throttle low (latch cleared);
  - the data-ready reason and its priority over KILL;
  - the ISR model end to end;
  - a latch left over while DISARMED, cleared by the normal arm sequence;
  - `wd_dead`;
  - recovery forbidden while ARMED (`to_safe` and `to_boot` refused, `recovery_allowed`);
  - DISARMED → SAFE → DISARMED after recalibration;
  - BOOT final;
  - the backup tick check (3 ms alone does not cut; the 6 ms backup does).
  - The randomised run now includes the latch and the watchdog: 3000 episodes, 604 389 iterations, 1231 cuts (438 by the watchdog, 116 heartbeat injections). At every iteration it checks: never ARMED with the latch set, data-ready missing, the backup tripped or the watchdog dead.
- **Output gate host test:** 566 535 checks, 0 failed.
  - Same header as the firmware.
  - The fake register layer defers a "watchdog interrupt" while masked.
  - The interrupt is fired at six points around the flight task's duty-and-release sequence, including inside each masked section and a resumed repeat of the write, plus 20 000 random interleavings.
  - Result: never an output on while latched.
  - Mutation check: with the latch check removed from `release_all()` the test reports 6 456 failures.
- **Seqlock and control frame host tests:** pass, unchanged.
- **`tools/analyse_m7b.py`:** checked on a synthetic log in the firmware's line formats, which found and fixed two parser bugs.

## 8b. Bench results (6 and 7 November 2026)
Logs: `logs/device-monitor-261106-194011.log` to `logs/device-monitor-261107-035524.log` (20 files, 104 boots). Bench build `esp32s3_bench`; no motors, 10 kΩ pull-downs on GPIO 4 to 7.

| # | Criterion | Result |
|---|---|---|
| 1 | Build, config check, host tests | Both environments 0 warnings; `verify_sdkconfig.sh` 38/38 and 39/39; safety 7 455 485 checks, output gate 566 535, seqlock and control frame (4043) pass |
| 2 | Heartbeat stall (`h`) | 22 cuts `wd_heartbeat`, 5008 to 5465 µs after the last heartbeat; gates low 1.44 to 1.49 µs after the decision. PASS |
| 3 | Missing data-ready (`m`) | 42 cuts `wd_imu_missing`, 3017 to 3496 µs after the last pulse (M7a: 4031 µs); gates low 1.43 to 1.91 µs. PASS |
| 4 | `H`, `T`, `X` (3 each) | `INT_WDT` 1.40 to 1.44 s, `TASK_WDT` 3.21 s, `PANIC` 1.21 s from hang to reset; each reboot DISARMED. 0 V on the meter during every backtrace (user, 7 November 2026); 0.07 V at throttle 100 while ARMED. PASS |
| 5 | Reset while ARMED (session 2, boot retry on) | 22 resets, all back to DISARMED, IMU initialised on the first attempt every time; 3 found SDA held low and cleared it; none stuck. PASS |
| 6 | Task layout | 15 tasks; Core 1 runs only flight, IDLE1, ipc1; Core 1 interrupts: GPIO, I2C_EXT0, TG0_T0 (level 3), tick, FROM_CPU; interrupt watchdog int 24 level 4. PASS |
| 10 | IMU recovery | 67 recoveries, all OK in 104.1 to 104.6 ms, none while ARMED; `z` while ARMED cut by `wd_imu_missing` first. Restart path once: 3 failed attempts, controlled restart 1/2, then `FLIGHT: FAIL` in BOOT. PASS |
| 11 | Critical-section A/B | ON and OFF, 10 000 frames per transport each: 0 give-ups, 0 torn on Core 1 both ways. Removed (`kSetpointCriticalSection = false`, approved) |
| 13 | Core 1 evidence | `nm -u` on the final build: no log, printf, heap, guard or atomic calls in libflight, libsafety, libhal_mcpwm, libwdt, libhal_gptimer; only `abort` (key `X`). The watchdog interrupt path disassembles to IRAM/ROM code and DRAM data only. PASS |
| 14 | 10 minutes ARMED under traffic | 688.3 s (UDP control plus WebSocket pings) and 618.6 s (after the fixes): no cut, no reset, 0 give-ups. PASS |

**Found and fixed during the bench:**
- **False "IMU missing" from a wrapped age** (`flight.cpp`): the data-ready timestamp was read after the burst read, so a pulse newer than the wake stamp made `woke - last_pulse` wrap to about 4295 s. It caused 92 false recoveries while not ARMED, and could have caused a false backup cut while ARMED (never seen). Fixed by clamping negative ages to 0; the analyser now fails any wrapped age.
- **Analyser task count:** the task-line regex missed priorities with a space (`3/ 3`) and reported 7 of 15 tasks. Fixed.
- **Seqlock host test:** reported 1 torn read in about half the runs. The slot's initial all-zero value, correctly returned by a read that overlaps the first write, failed the test's check-word test. Test fixed (12 of 12 runs pass); `seqlock.h` unchanged.
- **`CONFIG_GPTIMER_ISR_CACHE_SAFE=y`** added (approved): the watchdog interrupt was up to 902 µs late while DISARMED.

**Open, carried into M7c:**
- **Watchdog entry delay in the first 10 s after boot:** still 600 to 665 µs in the first window of each boot with the cache-safe setting, so the cause is not the flash cache; it lines up with Wi-Fi/PHY start-up. While fully ARMED the worst case over 196 windows was 123 µs. Arming takes at least the 2 s calibration after boot, but a window does not show when inside it the delay happened.
- **Brownout resets:** 19 in early sessions (one while ARMED), all at Wi-Fi start-up or when the laptop joined the AP, none after the supply was changed. Each was counted in RTC memory and came back DISARMED or re-browned out in BOOT. The supply and brownout level need measuring before motors (blueprint O4, §4.9 "level chosen by measurement").
- **The boot retry's second attempt** never ran on the bench (every first attempt succeeded).

## 9. Files
```
components/hal_gptimer/   CMakeLists.txt, include/hal_gptimer.h, hal_gptimer.cpp        (new)
components/wdt/           CMakeLists.txt, linker.lf, include/wdt.h, wdt.cpp              (new)
components/hal_mcpwm/     include/output_gate.h (new), linker.lf (new), hal_mcpwm.cpp, include/hal_mcpwm.h, CMakeLists.txt
src/m7b_bench.h, src/m7b_bench.cpp                                                      (new)
sdkconfig.defaults.bench                                                                (new)
tools/host_test/test_output_gate.cpp, tools/analyse_m7b.py                             (new)
docs/M7b_package.md                                                                     (new)
```
Also changed:
- `components/board_config/include/board_config.h`: M7b constants, `FE_M7B_TEST`, the banner;
- `components/safety`: reasons, inputs, `to_safe`, `to_boot`, `published_state`, `recovery_allowed`, `RestDetector::reset`;
- `components/flight`: watchdog, latch, recovery, boot retry, faults;
- `components/drv_mpu6500`: `last_notify_us` in IRAM, `sleep`, `soft_reset`;
- `components/hal_i2c`: `always_clear`, `kClocked`, `bus_reset`;
- `components/comms/link.cpp`: the A/B switch;
- `src/main.cpp`, `src/CMakeLists.txt`;
- `sdkconfig.defaults`, `platformio.ini`, `tools/verify_sdkconfig.sh`;
- `tools/host_test/test_safety.cpp`;
- `CLAUDE.md`.
