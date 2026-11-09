# Module 7c package: timing under load, R5 experiments and soak

**Status:** bench complete (9 November 2026): acceptance 1, 7, 8, 9, 12 and 13 pass (§8d). Decisions pending: the R5 default, B-M7c-08, the blueprint diffs. Implemented and built 7 November 2026. All six environments build with 0 warnings, the config check passes for each, and the five host tests pass (§8). Bench run pending. Blueprint diffs B-M7c-01 to 07 are proposed, not applied (§6).

M7c is the second half of the M7 spec (`docs/M7b_package.md` §1.1, approved split): acceptance 7, 8, 9 and 12, plus 1 and 13 for the new code.

## 0. Safety boundary
- No motors, MOSFET stage or shield. 10 kΩ pull-downs from GPIO 4 to 7 to GND, multimeter only.
- The bench duty cap (200 permille) and the start-up banner stay. The banner now reads "M7c bench firmware: no motors may be connected".
- Every M7b protection stays active in every M7c build:
  - the GPTimer watchdog and hard latch;
  - the interrupt and task watchdogs with panic force-low;
  - IMU recovery, the boot retry, the reset and brownout counters;
  - all M7b keys.
- The new keys (`0` to `4`, `n`, `L`) are read from UART0 by the M7b keys task only.
  - They label conditions and switch the synthetic load.
  - None can arm or drive an output.
  - Nothing on the network reaches them (checked: their only callers are in `src/m7b_bench.cpp` and `src/m7c_timing.cpp`).
- Supply: board straight into a laptop USB port with a short cable (the M7b brownouts disappeared after this change). Any BROWNOUT reset is counted and reported.

## 1. Spec

### 1.1 Decisions (approved 7 November 2026)
| ID | Decision |
|---|---|
| D1 | The M7c bench is the M7b bench plus `src/m7c_timing.{h,cpp}`. `FE_M7C_TEST` replaces `FE_M7B_TEST`; `FE_M7B_BENCH` builds the shared bench for either |
| D2 | Histograms as in §1.3 (loop period as the deviation from 1000 µs, 1 µs bins) |
| D3 | Condition keys `0` to `4`, `n` and `L` (§1.4) |
| D4 | R5 builds through project Kconfig options and per-environment overlays (§1.5); the (d) schedule reads the accelerometer at 250 Hz |
| D5 | The watchdog's 10 s line now carries the time of the worst alarm delay; the boot-time delay is investigated, nothing changed without approval |

**Additions with the approval:**
1. (d) keeps separate burst histograms for the 6-byte and 14-byte reads. An accelerometer value older than 8 ms is stale for the rest, tilt and arm checks. The R5 rule states the burst average and the loop-work p99.9 and maximum for every configuration.
2. `latency_test.py --no-control` sends no control frames (confirmed, §1.7). The soak runs with the synthetic load on.
3. Acceptance 7: load off; acceptance 8: load on, page serving.
4. R5 runs: page serving 300 s and WebSocket 50 Hz 300 s per environment, load off; the recommended configuration also gets 300 s of page serving with the load on.
5. IRAM and internal RAM cost per R5 environment, and the (b) fragment checked for anything that disables the flash cache (§1.6).
6. All M7b protections stay active; no key is reachable from the network.
7. Blueprint diffs collected, not applied.

### 1.2 Data flow (Core 1 to Core 0)
- **Core 1, every iteration:**
  - The flight task updates a static `flight::Timing` set (2992 B) with integer adds.
  - Before each wait, outside the measured work, it publishes the set through a `SeqlockSlot<Timing>` every 1 s, or at once when Core 0 asks.
  - The copy takes a few µs once per second. If a data-ready pulse arrives during it, that wake-up is a few µs later; the wake-up histogram includes this.
- **Core 0 requests**, each a counter with a single writer, so plain loads and stores:
  - `timing_reset()` starts a new condition epoch. Core 1 clears the set in place at its next iteration; no 3 KB temporary goes on the flight task's stack.
  - `timing_publish()` asks for an immediate publish. Core 0 waits up to 200 ms for it, so the final figures of a condition are complete.
  - `request_load()` switches the synthetic load.
- **Build flag:** `CONFIG_FE_LATENCY_HIST`, set only in `sdkconfig.defaults.bench`. Without it none of this is compiled. The flight build `esp32s3` has it off (checked).

### 1.3 Histograms (`components/latency/include/latency_hist.h`, header-only, host-tested)
| Histogram | Measures | Bins |
|---|---|---|
| Loop period | \|wake-to-wake − 1000 µs\| over consecutive data wake-ups; signed min and max kept | 1 µs, 0–199, plus overflow |
| Wake-up | data-ready interrupt timestamp to task wake | 1 µs, 0–99, plus overflow |
| Burst 14 | the 14-byte read | 10 µs, 0–1499, plus overflow |
| Burst 6 | the 6-byte gyro read, (d) builds only | 10 µs, 0–1499, plus overflow |
| Loop work | wake to the end of the iteration, every iteration, synthetic load included | 10 µs, 0–999, plus overflow |

- **How the figures are reported:**
  - Each histogram keeps its exact max and its sum, so averages are exact.
  - A percentile is the top of its bin, capped at the max: an upper bound within one bin of the true nearest-rank value. In the overflow bin it is the exact max.
- **Spec issue:** "loop period, 10 µs bins up to 200 µs" can't hold a ~1000 µs period. As a deviation, the M3–M7b jitter of about 5 µs would all land in the first 10 µs bin, hence 1 µs bins (D2).
- **Missed samples:** notifications beyond one per wake-up, as in M3 to M7b. Timeouts (waits without data) and I²C errors are counted separately.

### 1.4 Conditions and the synthetic load (`src/m7c_timing.cpp`)
- **Keys:**
  - `0` idle AP: laptop joined, no traffic.
  - `1` UDP 50 Hz.
  - `2` WebSocket 50 Hz.
  - `3` page serving.
  - `4` soak.
  - `n` ends the running condition. Starting one also ends the previous one.
- **What a key does:** it only labels the log; you start the matching traffic tool.
- **Output while a condition runs:**
  - a `TM <cond> 60s` line every 60 s (cumulative for the condition);
  - a `TM <cond> END` line when it ends.
  - Each line carries the R5 configuration and the load. If the load was toggled inside a condition, the line says `MIXED` and the analyser ignores it.
- **`L`:** switches a busy-wait of `board::kSyntheticLoadUs` = 150 µs after each IMU read, on the cycle counter. 150 µs is the budget for filters, attitude, PID and mixer (blueprint §4.3).
- **Boot line:** `M7C:` states the histograms, the R5 configuration (from the options themselves, not the environment name), the I²C path, the cache size, the read schedule, the load and the staleness limit.

### 1.5 R5 builds
Kconfig options (`components/board_config/Kconfig`) are used, not `build_flags`, because PlatformIO `build_flags` reach `src/` only and the switches are read by components and by a linker fragment. Each R5 environment adds one overlay after the bench overlay. The R5 environments are not in `default_envs`; build each with `-e`.

| Env | Config | Overlay | `.iram0.text` | Internal RAM used vs bench | Notes |
|---|---|---|---|---|---|
| `esp32s3` | flight build | — | 106 311 B | −13 856 B | no histograms, no trace facility |
| `esp32s3_bench` | (a) baseline | `.bench` | 109 055 B | 0 | heap starts at 0x3FCAE938 |
| `esp32s3_r5b` | (b) I²C transfer path in IRAM | `.r5b` | 116 691 B | **+7 424 B** | `CONFIG_FE_R5_I2C_IRAM=y` |
| `esp32s3_r5c` | (c) 32 KB instruction cache | `.r5c` | 109 055 B | **+16 384 B** | `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y` |
| `esp32s3_r5d` | (d) 6-byte gyro read, accelerometer 250 Hz | `.r5d` | 109 015 B | **−248 B** | `CONFIG_FE_R5_GYRO_ONLY=y` |
| `esp32s3_r5e` | (e) b + c + d (to be redefined from the results) | `.r5e` | 116 703 B | **+23 832 B** | all three |

- **How the cost was measured:** internal RAM used = the shift of `_heap_start` against the bench build. On the ESP32-S3, IRAM, static DRAM and the instruction cache share the internal SRAM, so this is the whole static cost.
- **Not yet measured:** the run-time internal free heap of each build is on the `main: HB` lines.
- **Bench vs flight build:** the bench's 13.9 KB over the flight build is the histograms (2 × 3.0 KB sets plus the 6.0 KB seqlock slot) and the trace facility.

**(b) IDF 5.5.3 options found:**
- `CONFIG_I2C_ISR_IRAM_SAFE=y` and `CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=y`, both already on, cover the interrupt and the command helpers only (`s_i2c_read_command`, `s_i2c_start_end_command`, `i2c_hal_master_trans_start`).
- No option places the synchronous master transfer in IRAM.
- So (b) is the conditional linker fragment `components/hal_i2c/linker.lf`:
  - `if FE_R5_I2C_IRAM = y:` with the `noflash_text` scheme;
  - for the IDF `i2c_master` object, `libhal_i2c` and `libdrv_mpu6500`.
  - Read-only data (log format strings) stays in flash.

**(d) schedule:**
- Every frame does the 6-byte read from GYRO_XOUT_H (0x43; verify against datasheet).
- Every 4th frame (`board::kR5AccelDivider`) does the full 14-byte burst instead. That frame's accelerometer and gyro come from one sample, so they stay in step.
- A failed full read is repeated on the next frame.
- Between full reads, the rest and tilt checks use the last accelerometer value.
- 250 Hz is well above twice the accelerometer's 41 Hz DLPF bandwidth (`ACCEL_CONFIG2` = 0x03), so the lower rate loses no information. It adds up to 3 ms of age to a value whose filter delay is already 11.8 ms.

**Accelerometer staleness (addition 1, every build):**
- An accelerometer value older than `board::kAccelStaleUs` = 8000 µs (two missed full reads at 250 Hz) is stale.
- While stale, `at_rest`, `accel_ok` and `tilt_ok` are false, so arming is refused (`accel_bad`, `moving`, tilt).
- In the 1 kHz builds a value goes stale only after 8 ms without a good read, when the IMU is already unhealthy.
- Staleness is not a cut while ARMED; the IMU-missing cuts already cover a stalled IMU.
- The `ST` line shows the accelerometer age, and each `TM` line counts the stale iterations.

**Recommendation rule (addition 1):**
- **The target:** the smallest change whose burst average under page serving, load off, is ≤ 517.5 µs (450 µs + 15 %).
- **The other numbers:** the loop-work p99.9 and max are stated for every configuration, beside the average, because (d) lowers the average but not the worst frame (a full read every 4th frame).
- **Decision:** the default changes only after your approval.

### 1.6 (b) placement check (addition 5)
In the linked `esp32s3_r5b` image:
- **Placement:** all 46 functions of the three mapped objects are in IRAM, none left in flash. Examples:
  - `i2c_master_transmit_receive` 0x42093D14 → 0x4037C978;
  - `s_i2c_synchronous_transaction` 0x4209309C → 0x4037BE00;
  - `s_i2c_transaction_start`, `s_i2c_send_commands`.
- **Direct calls from the moved code:** 56 distinct targets.
  - None is in flash.
  - None touches flash, cache, MMU, partitions or NVS (no `spi_flash_*`, `esp_flash_*`, `cache_*`, `Cache_*`, `mmu_*`, `esp_partition_*`).
  - They are FreeRTOS queue and semaphore calls, `esp_timer_get_time`, GPIO level calls, `periph_rcc_*`, `esp_pm_lock_*`, the heap and log calls of the init paths, and the I²C HAL.
- **Not resolvable statically:** the 44 indirect calls (`callx`, function pointers such as the driver's callbacks).
- **What the fragment does and doesn't do:** it places nothing that disables the flash cache. It is a speed experiment for a cache-enabled path. It doesn't make the transfer safe to run while the cache is off; the error-path format strings are still in flash.

### 1.7 Soak (acceptance 12)
- **`tools/run_m7b.py`:**
  - It reuses `control_client.Client`.
  - It repeats one cycle over UDP: throttle 0 with arm off for 0.7 s, an arm edge (0.1 s), throttle 100 for 20 s, then 1 s of silence, which triggers the expected `link_silence` cut at 150 ms.
  - It sends nothing else: no kill, no pings.
  - Dry run against a local fake device: 4 cycles, 4 arm edges, 4 silences, only control frames, no kill frame.
- **Pings:** `tools/latency_test.py --no-control` sends pings only. Its `send_loop` sends a control frame only when `control` is true. On the device, `link.cpp` returns on a ping before the setpoint write, so pings never touch a setpoint slot or count as a transport change. M7b step 13 showed WebSocket controls 0 with this flag.
- **Page serving:** `tools/m5_http_test.py --load 1200`.
- **Load:** on (`L`), per addition 2.

## 2. Runtime flash writes
None added.

## 3. Acceptance criteria (M7c)
1. **Builds and tests:**
   - all six environments build with 0 warnings;
   - `tools/verify_sdkconfig.sh` passes for each with its overlays;
   - the host tests pass: safety, output gate, seqlock, control frame, latency histogram.
7. **Jitter, baseline (`esp32s3_bench`), load off, 300 s per condition (idle AP; UDP 50 Hz; WebSocket 50 Hz; page serving):**
   - loop-period p99.9 < 100 µs;
   - wake-up max < 50 µs;
   - 0 missed samples and 0 I²C errors;
   - burst average and max reported.
8. **Baseline, load on (150 µs), page serving, 300 s:**
   - loop-period p99.9 < 100 µs and 0 missed;
   - loop-work max reported against 1000 µs.
9. **R5 table and recommendation:**
   - (a) to (e), each with page serving and WebSocket 50 Hz for 300 s, load off;
   - burst average (and max) and loop-work p99.9 and max stated for every configuration;
   - a recommendation with the rule in §1.5, plus 300 s of page serving with the load on for the recommended configuration.
12. **20-minute soak** (baseline, load on, UDP arm cycles, WebSocket pings and page serving together):
    - 0 missed samples and 0 I²C errors;
    - no unintended cut (only `link_silence`) and no reset; reset reasons listed and brownouts counted;
    - internal free heap within 2 KB over the soak;
    - stack high-water marks reported.
13. **Core 1 evidence repeated:**
    - no log, printf, heap, guard or atomic library calls in libflight, libsafety, libhal_mcpwm, libwdt, libhal_gptimer (only `abort`, key `X`);
    - the timing data is static DRAM;
    - `timing_clear` is in IRAM.

## 4. Bench procedure
**Session 1:** steps 1 to 7. Send the logs; I then propose (e) and the recommendation. **Session 2:** steps 8 to 10.

1. **Hardware:** as in M7b.
   - 10 kΩ from GPIO 4 to 7 to GND, multimeter on one of them.
   - No motors, MOSFET stage or shield.
   - Board flat and still, straight into a laptop USB port with a short cable.
2. **Build (laptop):**
   - `pio run` (builds `esp32s3` and `esp32s3_bench`), then `pio run -e esp32s3_r5b`, `-e esp32s3_r5c` and `-e esp32s3_r5d`.
   - Then `tools/verify_sdkconfig.sh`, and for each bench environment `tools/verify_sdkconfig.sh sdkconfig.<env> sdkconfig.defaults sdkconfig.defaults.bench [sdkconfig.defaults.r5x]`.
3. **Flash the baseline:** `pio run -e esp32s3_bench -t upload`. Monitor on the UART0 socket; type keys there only, one at a time.
4. **Boot checks:**
   - banner "M7c bench firmware";
   - `M7C: latency histograms on | R5 config a | ... 14-byte burst every frame | synthetic load 150 us (key L), off`;
   - `FLIGHT: PASS`, `WDT: PASS`, `TASKS: PASS`;
   - no `BROWNOUT` in the `RESET:` line.
5. **Acceptance 7, load off.** Join the laptop to the AP first. For each condition, press its key, run the tool to the end, then press `n`:
   1. `0`, then wait 300 s, then `n` (idle AP; no client, the laptop stays joined).
   2. `1`, then `.venv/bin/python tools/latency_test.py --transport udp --count 15000` (300 s at 50 Hz), then `n`.
   3. `2`, then `.venv/bin/python tools/latency_test.py --transport ws --count 15000`, then `n`.
   4. `3`, then `.venv/bin/python tools/m5_http_test.py --load 300`, then `n`.

   Baseline steps 2 to 4 double as the (a) rows of the R5 table.
6. **Acceptance 8, load on:**
   1. `L` (expect `KEY L: synthetic load ON`).
   2. `3`, then `m5_http_test.py --load 300`, then `n`.
   3. `L` again (load off).
7. **R5 (b), (c), (d).** For each of `esp32s3_r5b`, `esp32s3_r5c`, `esp32s3_r5d`:
   1. Flash it (`pio run -e <env> -t upload`) and check the `M7C:` line names the configuration.
   2. Rejoin the laptop.
   3. `3`, `m5_http_test.py --load 300`, `n`.
   4. `2`, `latency_test.py --transport ws --count 15000`, `n`.
8. **(e) and the recommended configuration** (session 2, after I've proposed (e) from the session 1 table):
   1. Flash `esp32s3_r5e` and run step 7's two conditions.
   2. Then, on the recommended configuration: `L`, `3`, `m5_http_test.py --load 300`, `n`, `L`.
9. **Soak (acceptance 12), on the baseline:**
   1. Flash `esp32s3_bench`, rejoin the laptop, press `L` (load on), then `4`.
   2. Within a few seconds, start in three terminals:
      - `.venv/bin/python tools/run_m7b.py --transport udp --minutes 20`;
      - `.venv/bin/python tools/latency_test.py --transport ws --no-control --count 60000`;
      - `.venv/bin/python tools/m5_http_test.py --load 1200`.
   3. When all three have finished, press `n`, then `s` (the task table with the stack high-water marks).
   4. Optional meter check during the soak: about 0.07 V while ARMED at throttle 100, 0 V after each silence cut.
10. **Analyse:** `python3 tools/analyse_m7b.py logs/device-monitor-<...>.log ...` with all the session's logs in order. Send the output and the latency-test summaries.

## 5. Not testable without hardware
Acceptance 7, 8, 9 and 12, and every timing figure:
- the R5 effects and the run-time free heap of each build;
- the boot-time watchdog delay and what it lines up with;
- whether the 1 s publish shows in the wake-up histogram;
- the (d) schedule on the real IMU (the 6-byte read's duration; GYRO_XOUT_H = 0x43 to be verified against the datasheet).

## 6. Proposed blueprint diffs (not applied)
| ID | Section | Old | New | Reason |
|---|---|---|---|---|
| B-M7c-01 | §4.3 timing budget | "Filters, attitude, PID, mixer < 150 µs, to be measured"; margin ≥ 350 µs | The M7c measurements: loop work p99.9 and max with the 150 µs synthetic load under page serving; margin stated from the max | Acceptance 8 (pending bench) |
| B-M7c-02 | §4.8 arm conditions | Tilt, rest and accelerometer checks on the current sample | An accelerometer value older than 8 ms is stale: the rest, tilt and accelerometer checks fail (arming refused); not a cut | Addition 1 |
| B-M7c-03 | §4.9 | — | Bench-only Kconfig options `CONFIG_FE_LATENCY_HIST`, `CONFIG_FE_R5_I2C_IRAM`, `CONFIG_FE_R5_GYRO_ONLY` (`components/board_config/Kconfig`), set only by `sdkconfig.defaults.bench` and `sdkconfig.defaults.r5b` to `r5e`; never in `sdkconfig.defaults` | D4 |
| B-M7c-04 | §6.2 R5 | Candidate fixes in M7 | The R5 table (a) to (e) with the burst average and the loop-work p99.9 and max, the IRAM and RAM cost of each, and the recommendation | Acceptance 9 (pending bench) |
| B-M7c-05 | §5 Phase 1, M7c row | "Loop jitter p99.9 < 100 µs" | Loop jitter defined as \|wake-to-wake − 1000 µs\| over consecutive data wake-ups, 1 µs bins; per condition (idle, UDP, WebSocket, page serving), load off; and with the 150 µs load under page serving | D2 spec issue |
| B-M7c-06 | §4.10 | Repository layout | Add `components/latency/` (header-only histograms), `src/m7c_timing.cpp`, `tools/run_m7b.py`, `tools/analyse_m7b.py` | Layout |
| B-M7c-07 | §4.4 signal chain (conditional) | Accelerometer read at 1 kHz in the 14-byte burst | Only if (d) is recommended and approved: the 6-byte gyro read at 1 kHz and the 14-byte burst every 4th frame (accelerometer 250 Hz, same-sample gyro) | Acceptance 9 |
| B-M7c-08 | §4.8 other layers (B-M7b-04) | Task watchdog 2 s with panic, watching IDLE0 and IDLE1 | Bench builds, temporary: IDLE0 not watched (`sdkconfig.defaults.bench`); IDLE1 watched with panic. Flight build unchanged until decided | Page serving starved IDLE0 for > 2 s: 23 `TASK_WDT` resets in a loop (7 November 2026, 13:21 and 13:59 logs). Open: plain Core 0 load, or a Core 0 hang (the first trip's backtrace was in a spinlock acquire; see the LittleFS crash in §8b) |
| B-M7c-09 | §4.6 communications, R3 | WebSocket and the static pages share one `esp_http_server` task | Recorded limitation: under continuous page serving (20 requests/s) the WebSocket replies queue behind the page sends. In the soak the device read 56 903 pings but the laptop got only 101 replies in time; the rest arrived later than 200 ms or not at all. Proposal (later module): no page serving while ARMED, or a second server instance for `/ws` | Soak, 9 November 2026 |
| B-M7c-10 | §4.2 rule 2 | — | The Wi-Fi PHY calibration, at about 0.79 s after start, holds off the Core 1 watchdog interrupt for 513–615 µs once per boot. The board is still in BOOT or SAFE then, never ARMED; no change | D5 |
| B-M7c-11 | §4.9, R5 | I²C master transfer from flash | Recommended: `CONFIG_FE_R5_I2C_IRAM=y` (the R5 (b) fragment) in the flight build, +7.4 KB internal RAM. Pending approval | Acceptance 9 |

## 7. Spec items that do not fit ESP-IDF 5.5.3 (or the spec) as written
1. **Loop-period bins:** "10 µs bins up to 200 µs" can't hold a ~1000 µs period; it's measured as the deviation in 1 µs bins (D2).
2. **No IDF option puts the synchronous I²C master transfer in IRAM.** The two existing options cover the interrupt and helpers only, so (b) is a linker fragment (§1.5).
3. **PlatformIO `build_flags` reach `src/` only**, so the R5 switches and the histogram flag are Kconfig options.
4. **`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` costs 16 KB of internal SRAM.** Measured: `_heap_start` moves by 0x4000.
5. **The M7b bench is extended, not forked (D1).** The analyser keeps its name, `analyse_m7b.py`, and accepts both banners.
6. **Extra histogram:** the loop-work histogram was added for acceptance 8. The spec asked only for the max.

## 8. Development results (7 November 2026, no hardware)
- **Builds:** all six environments, 0 warnings.
  - `sdkconfig.esp32s3*` were deleted and regenerated after the overlay changes.
  - `verify_sdkconfig.sh`: `esp32s3` 38/38, `esp32s3_bench` 40/40, `r5b`, `r5c`, `r5d` 41/41 each, `r5e` 43/43.
  - The generated configs show each option only where intended:
    - `FE_LATENCY_HIST` in the five bench builds;
    - `FE_R5_I2C_IRAM` in r5b and r5e;
    - `FE_R5_GYRO_ONLY` in r5d and r5e;
    - the 32 KB cache in r5c and r5e.
- **Host tests:**
  - safety 7 455 485 checks, 0 failed;
  - output gate 566 535, 0 failed;
  - seqlock PASS;
  - control frame 4043, 0 failed;
  - **latency histogram (new) 7211 checks, 0 failed.** It covers:
    - percentiles against an exact sort for all four bin layouts, 300 random runs each;
    - overflow and the exact max;
    - 64-bit rank arithmetic.

    Mutation checks: without the cap at the max, 124 failures; with the rank rounded down, 1188.
- **Core 1 evidence (acceptance 13), bench, r5d and r5e builds:**
  - `nm -u` shows no log, printf, heap, guard, atomic or double-precision calls in libflight, libsafety, libhal_mcpwm, libwdt, libhal_gptimer; only `abort` in libflight (key `X`).
  - The Core 1 set `flight::s_tm` (2992 B) and the slot (5992 B) are in DRAM `.bss`.
  - `flight::timing_clear` is in IRAM (0x4037DCD8).
- **(b) placement:** §1.6.
- **Tools:**
  - `run_m7b.py`: dry run against a local fake device (§1.7).
  - `analyse_m7b.py`, M7c section: checked on a synthetic log built from the firmware's own `TM` format string. It produced the condition table, acceptance 7 and 8, the R5 table, the soak verdict, and caught a planted unintended cut. Mixed-load and short runs are flagged.

## 8b. Bench session 1 findings (7 November 2026)
- **Acceptance 7, baseline, load off** (13:59 log, after the laptop's background Wi-Fi scan was turned off):

  | Condition | Period p99.9 | Wake-up max | Missed | I²C errors | Burst avg/max |
  |---|---|---|---|---|---|
  | idle | 4 µs | 20 µs | 0 | 0 | 453/714 µs |
  | UDP | 6 µs | 21 µs | 0 | 0 | 473/758 µs |
  | WebSocket | 7 µs | 21 µs | 0 | 0 | 519/769 µs |

  All three pass. Latency tool: UDP p95/p99 1.84/2.37 ms, WebSocket 3.69/5.39 ms, no loss.
- **Laptop background Wi-Fi scan:** before it was turned off, it caused periodic outages: 1–2 s every 15 s and 4–5 s every 75 s, on both transports. The device turnaround was normal throughout.
- **Page serving reset the board in a loop:** `TASK_WDT` with IDLE0 starved, and `wifi`, `tiT` or `httpd` running on Core 0. 23 resets in the 13:59 log, 29 trips in the 05:06 log. Temporary bench change: B-M7c-08.
- **A web-server crash that predates M7c:** LoadProhibited (EXCCAUSE 0x1C), a garbage lock pointer (0xB171C428) in `vfs_littlefs_pread` under `open()` in `http_static.cpp:42`, on Core 0 during page serving.
  - The same crash, with the same register pattern, is in the M5 log of 31 October (`device-monitor-261031-200524.log`), together with a heap-corruption panic (`block_set_free`).
  - It is not caused by M7c and is still open.
  - It has to be understood before acceptance 7 (page), 8 and 12 can pass.

### R5 results so far (7 November 2026; load off; figures over the 300 s load window)
| Config | Page: burst avg / max | Page: loop work p99.9 / max | WS: burst avg | WS: loop work p99.9 / max | Internal RAM | Burst target ≤ 517.5 µs |
|---|---|---|---|---|---|---|
| (a) baseline | 620 / 775 µs | 749 / 783 µs | 519 µs | 759 / 783 µs | 0 | missed |
| (b) I²C path in IRAM | 479 / 540 µs | **539 / 553 µs** | 462 µs | 539 / 545 µs | +7.4 KB | met |
| (c) 32 KB instruction cache | 499 / 663 µs | 639 / 669 µs | 472 µs | 629 / 663 µs | +16.0 KB | met |
| (d) 6-byte gyro read | 481 µs (6 B: 433 / 576, 14 B: 623 / 759) | 729 / 773 µs | 386 µs | 739 / 773 µs | −0.2 KB | met |

- **Every configuration:** 0 missed samples, 0 I²C errors, period p99.9 ≤ 17 µs, wake-up max ≤ 21 µs.
- **Which line each figure comes from:** the (c) and (d) conditions and the (b) page condition were ended late or not at all. Their figures come from the cumulative 60 s line at t ≈ 300 s, which covers the load window. Maxima are unaffected either way.
- **(d):** it lowers the average but not the worst frame, because the full read every 4th frame still takes up to 759 µs. Its 6-byte read averaged 433 µs under page serving, not the estimated 260 µs.
- **(e) redefined:** (b) + (c), without (d). Internal RAM +23.3 KB; config check 42/42.
- **Provisional recommendation:** (b), the smallest change that meets the target. It also gives the largest cut to the worst frame: loop-work max 553 µs against 783 µs, about 230 µs freed. Pending (e) and the run of (b) with the load on.

## 8c. Diagnostic builds for the Core 0 crash (approved 7 November 2026)
- **What happened (14:42 log, `esp32s3_bench`, IDLE0 unwatched):**
  - No task-watchdog trips, but two resets during page serving, about 194 s in and about 85 s after the reboot.
  - Both were `TG1WDT_SYS_RST` with the saved program counter in the panic handler's backtrace walk (`esp_backtrace_get_next_frame`). A panic started, the handler hung on a corrupted stack before printing, and the hardware watchdog reset the board.
  - Core 1 was clean up to the crash: period p99.9 13 µs, 0 missed.
  - The 1 s `main` heartbeat went missing for up to 9 s under the load: Core 0 is saturated.
- **`esp32s3_heapdiag`:** the bench build plus `sdkconfig.defaults.heapdiag`:
  - comprehensive heap poisoning;
  - the strong stack protector;
  - `heap_caps_check_integrity_all(true)` from the Core 0 bench task every second, with a `HEAPCHK` line every 10 s, or at once on a failure.
  - The IRAM code grows by 3.9 KB.
- **`esp32s3_heapdiag_nommap`:** the same, plus `sdkconfig.defaults.nommap` (memory-mapped LittleFS off). This is the isolation step left in `docs/M5_package.md`. Its Core 1 timings don't count.
- **Both:** a `DIAG:` boot line states the build. Bench only, never flight. Config check: 42/42 each.
- **First attempt (15:00 and 15:02 logs):** an interrupt-watchdog panic on Core 0 about 1 s after every boot, in both builds. The cause was the diagnostic itself: `heap_caps_check_integrity_all()` verified the poisoning fill of the whole 8 MB PSRAM heap with the heap lock held for more than 100 ms. The check was limited to internal RAM (`heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true)`, about 1.9 ms per walk). The 15:00 log also has one BROWNOUT reset at Wi-Fi start-up.
- **Second attempt, after you changed to a better USB cable (15:17 heapdiag, 15:23 heapdiag_nommap):**
  - Each ran 300 s of page serving with no crash, no reset and no brownout.
  - The heap checks found no corruption: 310 and 350 walks, 0 failures. No stack-smashing report.
  - HTTP client: 6000 page and 150 file requests, 1 connection error in each run, with nothing in the device log.
- **Core 1 under page serving:**
  - heapdiag build: period p99.9 14 µs (max 29 µs), wake-up max 22 µs, 0 missed, burst average 631 µs.
  - nommap build: period max 1003 µs, wake-up max 256 µs and 455 overruns, because file reads stop the cache. That confirms D-M5-04: memory-mapped LittleFS stays on.
- **Reading:** the crashes stopped when the cable changed, in both builds, and the 15:00 brownout shows the old supply path dipping under Wi-Fi transmit. This supports M5's power-integrity explanation. It isn't proven, because the diagnostic builds also change Core 0 timing. The decisive check is the plain `esp32s3_bench` page-serving run on the new cable.

## 8d. Bench results, sessions 1 and 2 (7 to 9 November 2026)
Analyser over every valid M7c log: `135922`, `154912`, `161107`, `170547`, `232235`, `233517` (7 November), `025859`, `032844`, `034111` (9 November).

| # | Result |
|---|---|
| 7 | PASS, baseline, load off. Idle: period p99.9 4 µs, wake-up max 20 µs. UDP: 6 / 21 µs. WebSocket: 7 / 21 µs. Page serving: 12 / 21 µs. 0 missed and 0 I²C errors throughout. Burst avg/max 453/714, 473/758, 519/769 and 620/775 µs |
| 8 | PASS, baseline, page serving, load 150 µs (`161107`, the 300 s line; the condition wasn't ended with `n`): period p99.9 9 µs (max 18 µs), 0 missed, burst 622/765 µs, loop work p99.9/max 889/929 µs: **71 µs left in the 1000 µs frame** |
| 9 | Table below. Recommendation: **(b)** |
| 12 | PASS, baseline, load on: 1267 s with UDP arm cycles, WebSocket pings and page serving at once. 0 missed, 0 I²C errors. 40 arm cycles, 40 expected link-loss cuts, no other cut, no reset, no brownout. Period p99.9 9 µs, loop work max 938 µs. Internal heap first/last 185 895/185 783 B (drift −112 B), minimum 153 303 B under load. Lowest stack headroom: ipc1 468 B, ipc0 540 B, IDLE1 732 B, flight 3292 B, httpd 3536 B |
| 13 | PASS (§8) |

**R5, page serving, load off (WebSocket in brackets):**

| Config | Burst avg / max | Loop work p99.9 / max | Internal RAM |
|---|---|---|---|
| (a) | 620 / 775 µs (519 / 769) | 749 / 783 µs (759 / 783) | 0 |
| (b) | 479 / 540 µs (462 / 532) | 539 / 553 µs (539 / 545) | +7.4 KB |
| (c) | 499 / 663 µs (472 / 653) | 639 / 669 µs (629 / 663) | +16.0 KB |
| (d) | 481 µs combined (386) | 729 / 773 µs (739 / 773) | −0.2 KB |
| (e) = b + c | 455 / 509 µs (453 / 501) | 499 / 516 µs (499 / 512) | +23.3 KB |
| (b), load 150 µs | 478 / 537 µs | 689 / 703 µs: **297 µs left** | +7.4 KB |

**Recommendation (rule in §1.5): (b), the IMU I²C transfer path in IRAM.**
- It's the smallest change that meets the burst target.
- It frees 230 µs at the worst frame. With the 150 µs load, the margin grows from 71 µs to 297 µs.
- (e) gains a further 37 µs for another 16 KB of internal RAM. That's worth keeping in reserve if the real estimator needs it.
- (d) doesn't help the worst frame.

**Other findings:**
- **The crashes came from the supply:** the M5 and M7c crashes under page serving stopped with a better USB cable (§8c). There were no crashes or brownouts in about 50 minutes of page serving afterwards.
- **WebSocket under page load:** B-M7c-09.
- **Boot-time watchdog delay:** B-M7c-10.
- **`latency_test.py` printed `RESULT: PASS` at 99.8 % loss:** its result ignores the loss flag. A tool fix is proposed.

## 9. Files
```
components/latency/        CMakeLists.txt, include/latency_hist.h                    (new)
components/board_config/   Kconfig (new), include/board_config.h
components/hal_i2c/        linker.lf (new), CMakeLists.txt
components/drv_mpu6500/    drv_mpu6500.cpp, include/drv_mpu6500.h   (read_gyro)
components/flight/         flight.cpp, include/flight.h, CMakeLists.txt
components/wdt/            wdt.cpp, include/wdt.h                   (worst-delay time)
src/m7c_timing.h, src/m7c_timing.cpp                                                  (new)
src/m7b_bench.cpp, src/m7b_bench.h, src/main.cpp, src/CMakeLists.txt
sdkconfig.defaults.bench, sdkconfig.defaults.r5b ... r5e (new), platformio.ini, .gitignore
tools/run_m7b.py, tools/host_test/test_latency_hist.cpp                               (new)
tools/analyse_m7b.py
docs/M7c_package.md                                                                   (new)
```
