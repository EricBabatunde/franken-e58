# Franken-E58 Phase 1, Module 3: MPU-6500 Driver with Interrupt-Driven Sampling

**Scope:** a `drv_mpu6500` component that configures the IMU and reads it on its data-ready interrupt, plus a bench test that measures the interrupt rate, wake-up latency, jitter, burst time and static values. No motors, no Wi-Fi, no filters or estimators yet.

Register addresses, bit fields and values come from `docs/mpu6500_notes.md`, which carries the datasheet page references (PS = product specification PS-MPU-6500A-01, RM = register map RM-MPU-6500A-00).

Section 1 records the module spec exactly as it was given. Section 2 lists the deviations that were approved before implementation; where they conflict with section 1, section 2 applies. Section 3 gives the acceptance criteria with those deviations applied.

---

## 1. Spec as given

> Module 3: MPU-6500 driver with interrupt-driven sampling, plus housekeeping. Read CLAUDE.md, docs/mpu6500_notes.md and the blueprint sections 3.3, 3.4, 4.2 to 4.4 first. Show a plan first and wait for approval. Never upload or open the serial monitor. Write docs/M3_package.md recording this spec, the bench procedure and the acceptance criteria exactly as given here, and set the CLAUDE.md status line to M3.
>
> **PART 0: HOUSEKEEPING (documentation and comments only, no behaviour change)**
> 1. Blueprint 2.3 heading: "Board verification (Module 1 boot log)" becomes "Board verification (Module 1 boot log, Module 2 bus test)".
> 2. Blueprint 2.2: the MPU interpretation text names the GY-521. Reword it to "IMU breakouts of this kind usually put the pull-ups on the regulated side of the onboard regulator". Keep the measurement and the optional check.
> 3. Blueprint 6.3: the INT-driven sampling row's reason "Clean Δt" becomes "Sensor-synchronised loop; Δt measured from the interrupt timestamp".
> 4. Blueprint 4.4 item 1 heading: replace "registers to be verified against the datasheet and register map" with "registers verified in docs/mpu6500_notes.md, which carries the page references".
> 5. Blueprint 4.3 note: the gyro-only read estimate becomes "about 260 µs (estimate: 81 clocks, about 200 µs wire time, plus about 60 µs driver overhead; measure before relying on it)".
> 6. Keep the M3 row's "8000 ± 80 Hz" but word it as "expected if the gyro drives data-ready; the measurement decides, not a pass threshold".
> 7. Replace "MPU-6050" with "MPU-6500" in docs/M2_package.md (goal 1, wiring labels, code listings), in source comments (hal_i2c.h, the kI2cHz comment in board_config.h) and in the log message "MPU-6050 not found" (change it to "MPU not found"). Keep the 0x68 or 0x70 WHO_AM_I handling in m2_i2c_test.cpp as it is.
>
> **PART 1: THE DRIVER AND THE BENCH TEST**
>
> New component drv_mpu6500 (C++17), plus src/m3_imu_test.cpp behind a new FE_M3_TEST flag in board_config.h. FE_M2_TEST and FE_M3_TEST must be mutually exclusive (use a compile-time #error). Reuse hal_i2c for every transfer. The driver never calls an IDF I2C function directly. Pins and constants live only in board_config. Add to board_config: kDlpfCfg (default 1), kIsrDecimation (default 1), gyro full scale (±1000 °/s), accel full scale (±8 g), accel DLPF setting (A_DLPF_CFG = 3), SMPLRT_DIV (0), with a static_assert that the decimation factor is consistent with kDlpfCfg (1 for DLPF_CFG 1 to 6; 8 only valid with DLPF_CFG 0). Every register address, bit field and value comes from docs/mpu6500_notes.md; keep its page references in comments, and mark anything not covered there "verify against datasheet".
>
> Init sequence (runs only at init, may allocate and log):
> - a. Probe 0x68 and read WHO_AM_I; accept only 0x70.
> - b. DEVICE_RESET (0x80 to PWR_MGMT_1), wait at least 100 ms, confirm PWR_MGMT_1 reads 0x01.
> - c. Write CONFIG (kDlpfCfg), GYRO_CONFIG = 0x10, ACCEL_CONFIG = ±8 g, ACCEL_CONFIG2 = 0x03, SMPLRT_DIV = 0, INT_PIN_CFG = 0x10. Read every register back and fail loudly on any mismatch. Never write 0x00 to PWR_MGMT_1.
> - d. Write INT_ENABLE = 0x01 last. Wait 35 ms after the reset before trusting gyro data.
>
> Driver API: init, reconfigure (repeat the full sequence with a different DLPF_CFG, used by the test), read_burst (one 14-byte transfer from 0x3B into a plain struct with a timestamp), scaling to °/s and g by constants. No heap allocation after init. All buffers static.
>
> Interrupt path:
> - GPIO 9, rising edge. gpio_install_isr_service and the handler are installed from a task pinned to Core 1, with ESP_INTR_FLAG_IRAM. The ISR is IRAM_ATTR and does only this: read esp_timer_get_time truncated to uint32_t, count the raw pulse, and on every kIsrDecimation-th pulse store the timestamp and call vTaskNotifyGiveFromISR. Timestamps are 32-bit with unsigned subtraction (wrap at about 71 minutes; document it). No 64-bit atomics anywhere. No mutexes, queues or semaphores between cores.
> - The bench task is pinned to Core 1, at the highest priority used so far. It also creates bus A and initialises the driver (as in M2 F5). It blocks on ulTaskNotifyTake, uses the returned count to detect missed samples (count above 1 means missed), then does the burst read.
> - After its init phase the Core 1 task must call no ESP_LOG or printf, allocate nothing, and take no lock shared with Core 0. It publishes statistics to a Core 0 reporter task through a seqlock slot (double buffer with a sequence counter) every 10 seconds.
>
> Bench test (about 12 minutes in one run):
> - Phase A, 60 s: reconfigure to DLPF_CFG = 0 and only count raw data-ready pulses (no burst reads). Report the pulse rate, the interval min, max and mean, and whether it stays steady. The datasheets do not say which sensor drives data-ready in the 8 kHz mode; this phase measures it. Record the 8000 ± 80 Hz figure as an expectation only.
> - Phase B, 600 s: full sequence at the compiled-in kDlpfCfg and kIsrDecimation. Per 10-second window report: pulse rate; ISR-timestamp interval min, max and standard deviation; burst duration min, avg, max; ISR-to-task wake-up latency min, avg, max in µs; missed samples; I2C errors; the accelerometer magnitude mean (should be near 1 g) and per-axis gyro mean and standard deviation at rest. Also report the maximum ISR execution time in CPU cycles, the Core 1 task stack high-water mark, and the free heap at the start and the end.
> - Then print one SUMMARY line for each phase. The reporter runs on Core 0.
>
> Report-only investigations (change no code):
> 1. Read the ESP-IDF 5.5.3 sources and report exactly what i2c_master_bus_reset() does, whether it allocates, and whether it is safe to call from a task on Core 1. This resolves the M3 item from D-M2-01.
> 2. Check gpio_isr_handler_add, ESP_INTR_FLAG_IRAM, vTaskNotifyGiveFromISR and esp_timer_get_time against the IDF sources. Confirm each is IRAM-safe and ISR-safe, and say which kernel locks the notification path takes.
>
> ACCEPTANCE (state these in docs/M3_package.md):
> 1. Clean build and tools/verify_sdkconfig.sh passes.
> 2. WHO_AM_I 0x70, PWR_MGMT_1 reads 0x01 after reset, every written register reads back correctly.
> 3. Phase B pulse rate 1000 ± 10 Hz (the datasheet gives ±1 % for the PLL).
> 4. Phase B: zero missed samples, zero I2C errors, one sample consumed per notification.
> 5. Phase A rate measured and reported.
> 6. Wake-up latency and interval jitter reported. Flag anything with a maximum above 200 µs.
> 7. Static values reported. Flag an accelerometer magnitude outside 0.95 to 1.05 g, and quote the gyro zero-rate tolerance from the datasheet with its page.
> 8. Heap unchanged between the start and end of Phase B.

## 2. Approved deviations

| ID | Deviation | Reason |
|---|---|---|
| D-M3-01 | The Core 1 init does not log. The driver returns an `InitStatus` (step, register, expected and actual values, WHO_AM_I, PWR_MGMT_1 after reset), the Core 1 task publishes it, and the Core 0 reporter prints it, with `ESP_LOGE` and a stop on any failure ("fail loudly"). Allocation during init on Core 1 stays allowed | CLAUDE.md forbids ESP_LOG on Core 1 at all times; only heap allocation is limited to "after initialisation". Same pattern as M2 F5 |
| D-M3-02 | Bench-only ISR instrumentation behind the compile-time flag `FE_IMU_ISR_INSTRUMENT` (on only when `FE_M3_TEST` is on): raw-interval min and max, the handler's own cycle count, and a runtime notify-every-N value (0 = count only). With the flag off, the flight ISR is timestamp, count, notify, with N fixed at `kImuIsrDecimation`. The cycle figure covers our handler only, not the IDF GPIO dispatcher that calls it | Phase A needs raw-interval statistics and a count-only mode; the spec asks for the ISR execution time |
| D-M3-03 | The free heap is sampled by the Core 0 reporter when it sees Phase B start and end, and the Core 1 task's stack high-water mark is read from Core 0 through the task handle | `heap_caps_get_free_size` takes the heap lock, which is shared with Core 0 |
| D-M3-04 | "Steady" means every 10 s window rate within 1 % of the phase mean | Not defined in the spec |
| D-M3-05 | Phase A is split into A1 and A2 (see below). The run is about 12.5 minutes | The datasheets do not say whether a new data-ready pulse is generated while the status flag is still set (docs/mpu6500_notes.md section 5) |
| D-M3-06 | Acceptance 7: the accelerometer magnitude band is 0.90 to 1.10 g, and gyro bias beyond 5 °/s is flagged | Worst case of the 60 mg zero-g offset and 3 % sensitivity tolerance (PS p.8). Initial zero-rate output tolerance ±5 °/s (PS p.7) |
| D-M3-07 | CLAUDE.md Core 1 rule and blueprint §4.2 rule 8 reworded: "Never share a user-level mutex, semaphore or queue with Core 0. The FreeRTOS kernel spinlock taken by task notifications is unavoidable; its effect on wake-up latency is measured in M3 (Wi-Fi off) and M7 (Wi-Fi on)." Blueprint risk R5 added: "Kernel spinlock contention delaying ISR-to-task wake-up under Wi-Fi load", measured in M3 and M7, fallback a Core 1 task waiting on an atomic flag set by the ISR | `vTaskNotifyGiveFromISR` and `ulTaskNotifyTake` both take the FreeRTOS `xKernelLock`, a spinlock shared by both cores |
| D-M3-08 | Blueprint §4.2 rule 9 records the `i2c_master_bus_reset()` result: allocation-free; takes the shared periph_spinlock briefly; on the ESP32-S3 the driver does not wait for the 9-pulse bus clear to finish, so the clear may be cut short; callable only by the task that owns the bus, between transfers; usable for recovery while DISARMED only | Investigation 1 |
| D-M3-09 | The two remaining MPU-6050 source comments (`board_config.h` bus A comment, `m2_i2c_test.cpp` register comment) are also renamed. The `OK: MPU-6050` label for WHO_AM_I 0x68 stays | Part 0 item 7 listed only some of them |

**Phase A as implemented (D-M3-05):**

- **A1, 10 s:** DLPF_CFG = 0, no burst reads, count raw pulses only. Report the raw rate and whether pulses continue after the first. If they stop, the report says so plainly.
- **A2, 60 s:** DLPF_CFG = 0, decimation 8. Wake the task on every 8th pulse and do the burst read, while the ISR still counts every raw pulse. Report the raw pulse rate, the read rate, interval min, max and mean, steadiness (each 10 s window within 1 % of the phase mean), and ISR-to-task latency.
- One SUMMARY line each for A1, A2 and B. Phase B is unchanged.

Each phase starts with a full `reconfigure()` (DEVICE_RESET and the whole sequence), so A1, A2 and B each begin from a known device state.

## 3. Acceptance criteria (with the approved deviations)

1. Clean build and `tools/verify_sdkconfig.sh` passes.
2. WHO_AM_I 0x70, PWR_MGMT_1 reads 0x01 after reset, every written register reads back correctly (printed for the initial init and for each reconfigure).
3. Phase B pulse rate 1000 ± 10 Hz (the datasheet gives ±1 % for the PLL, PS p.11).
4. Phase B: zero missed samples, zero I2C errors, one sample consumed per notification.
5. Phase A1 and A2 rates measured and reported.
6. Wake-up latency and interval jitter reported. Flag anything with a maximum above 200 µs.
7. Static values reported. Flag an accelerometer magnitude outside **0.90 to 1.10 g** (60 mg zero-g offset and 3 % sensitivity tolerance, worst case, PS p.8). Flag gyro bias beyond **5 °/s**, the initial zero-rate output tolerance (PS p.7).
8. Heap unchanged between the start and end of Phase B.

## 4. Bench procedure

1. Wiring is as in M2 (`docs/M2_package.md` §2). GPIO 9 must be connected to the IMU INT pin.
2. Build is clean and `tools/verify_sdkconfig.sh` passes (acceptance 1). `FE_M3_TEST` is 1 and `FE_M2_TEST` is 0 in `board_config.h`.
3. Plug in USB, upload, and open the monitor.
4. Expect, in order:
   - the M1 boot banner;
   - `M3 bench test: A1 10s, A2 60s, B 600s at DLPF_CFG 1, decimation 1`;
   - `init: WHO_AM_I 0x70, PWR_MGMT_1 after reset 0x01, DLPF_CFG 1, 7 registers written and read back OK`;
   - `reconfigure for A1: …`, then the A1 window line and `SUMMARY A1`;
   - `reconfigure for A2: …`, six A2 window lines and `SUMMARY A2`;
   - `reconfigure for B: …`, sixty B window lines, `SUMMARY B` and `SUMMARY B checks`;
   - `M3 bench test finished`.
5. Leave the board **still and level** for the whole run, so that the static values mean something.
6. Copy the three SUMMARY lines, the `SUMMARY B checks` line, and any error or warning line.

## 5. Files

```
components/drv_mpu6500/
├── CMakeLists.txt
├── include/drv_mpu6500.h
└── drv_mpu6500.cpp
src/
├── m3_imu_test.h
└── m3_imu_test.cpp      (bench test, deleted before Module 7)
```

Also changed: `board_config.h` (IMU constants, `static_assert`, `FE_M2_TEST 0`, `FE_M3_TEST 1`, `FE_IMU_ISR_INSTRUMENT`), `hal_gpio` (`imu_int_install`), `src/main.cpp`, `src/CMakeLists.txt`.
