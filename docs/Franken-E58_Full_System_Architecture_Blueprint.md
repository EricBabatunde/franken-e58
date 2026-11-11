# Franken-E58: Full System Architecture & Implementation Blueprint

**Revision:** 2.15. **Status:** the binding reference for all firmware, hardware and test work on the project. Items under §6.1 (Open Items) are not yet decided and carry stated defaults.

This document defines the hardware topography, software architecture and phased development lifecycle for an ESP32-S3-powered, closed-loop stabilised micro-quadcopter built on a Mavic E58 frame. The airframe's original flight controller and MOSFET board are removed completely. Only the camera is retained for now.

---

## 1. System Architecture Overview

The architecture decouples high-current actuation from low-signal sensing, and asynchronous networking from the deterministic flight loop.

- **Core 1** runs one hard-real-time task at 1 kHz: IMU read, filtering, attitude estimation, rate and angle control, mixing and motor output. It never allocates memory, never logs through `printf`, and never takes a lock shared with Wi-Fi.
- **Core 0** runs everything asynchronous: Wi-Fi AP, HTTP and WebSocket server, UDP receive, ToF sensor task, telemetry drain, battery sampling.
- **The safe state is hardware-backed.** Every motor gate has a 10 kΩ pull-down, so the drone is inert whenever the firmware is not actively driving it.
- **The firmware is written from the ground up** on ESP-IDF, as a learning path and to keep full control of timing. ESP-Drone and Crazyflie serve as reference material only (EKF, PID structure, mixer conventions).

**Operating principles**

1. **Modular iteration.** Every driver and task is proven in isolation on the bench before it enters the flight stack.
2. **Deterministic core.** No Core 1 code path may block on, or be delayed by, networking.
3. **Safe by default.** Props are off for all electrical and software bring-up. Propellers are fitted only from Phase 3 (thrust and vibration testing) onwards, under the restrictions stated there.

---

## 2. Hardware Inventory & As-Built Measurements

### 2.1 Inventory

| Item | Detail |
|---|---|
| MCU board | ESP32-S3-**N16R8** (16 MB flash, 8 MB **octal** PSRAM), 44-pin dev board |
| IMU | MPU-6500 (WHO_AM_I 0x70, confirmed on the bench in M2) on a breakout (silkscreen: VCC) |
| ToF | VL53L1X on a breakout (silkscreen: VIN) |
| Motors | 4× brushed coreless. Stall current unknown (measured in Phase 3) |
| Battery (bench) | 18650, protected high-drain cell. LiPo/LiHV flight pack chosen in Phase 4 |
| Instruments | Multimeter only. Optional INA219/INA226 breakout planned for Phase 3 |
| Frame | Mavic E58. OEM board removed. Camera retained |
| Dev machine | Ubuntu, VS Code, PlatformIO |

### 2.2 Measured breakout characteristics (power off, multimeter)

| Breakout | Measurement | Interpretation |
|---|---|---|
| VL53L1X | **10 kΩ** SDA→VIN and SCL→VIN | Onboard 10 kΩ pull-ups, tied to the supply. Powered at 3.3 V, so they reference 3.3 V. |
| IMU breakout (MPU-6500) | **Open** SDA→VCC and SCL→VCC | Inconclusive. IMU breakouts of this kind usually put the pull-ups on the regulated side of the onboard regulator, so the VCC pin cannot see them. Optional check: power from 3.3 V, then measure DC volts SDA→GND. About 3 V means pull-ups present, about 0 V means none. |

**Pull-up strategy.** Fit **4.7 kΩ external pull-ups to 3V3 on all four lines** on the breadboard. This is safe in every case:

| Bus | On-board | External | Combined |
|---|---|---|---|
| Bus A (IMU) | none or about 4.7 kΩ | 4.7 kΩ | 4.7 kΩ or about 2.35 kΩ |
| Bus B (ToF) | 10 kΩ | 4.7 kΩ | about 3.2 kΩ |

Valid window for 400 kHz Fast-mode at 3.3 V with about 50 pF bus capacitance:

$$t_r \approx 0.8473\,R_p C_b < 300\,\text{ns} \;\Rightarrow\; R_p \lesssim 7\,\text{k}\Omega, \qquad R_p \gtrsim \frac{V_{DD}-0.4\,\text{V}}{3\,\text{mA}} \approx 0.97\,\text{k}\Omega$$

All combinations above sit inside 1 to 7 kΩ. Final PCB pull-up values are chosen in Phase 2 with the same formula.

### 2.3 Board verification (Module 1 boot log, Module 2 bus test)

| Item | Result |
|---|---|
| Flash | 16 MB, generic chip, running DIO at 80 MHz (QIO enable failed) |
| PSRAM | 8 MB octal (AP, 64 Mbit), 40 MHz |
| CPU | Dual core, 240 MHz, chip revision v0.2 |
| Consoles | UART0 (GPIO 43/44) and native USB Serial/JTAG both print the same log. Native socket enumerates as `303a:1001` |
| Reset reason on power-up | Power-on, no brownout |
| I2C buses | Both verified at 400 kHz: 10-minute soak, zero errors on both (M2) |
| IMU timing | 1 kHz interrupt-driven: 999.74 Hz, interval standard deviation 0.44 µs, ISR-to-task wake 6 to 13 µs, 14-byte burst 450 µs, 10 minutes with zero missed samples (M3) |
| 8 kHz mode | DLPF_CFG 0 gives 7997.9 Hz data-ready with no reads; under read traffic one raw pulse was occasionally lost (raw interval up to 251 µs, read interval up to 1126 µs) (M3) |
| ToF | VL53L1X short mode, 20 ms budget: 39.68 Hz, valid 100 %, sd 1.5 mm at about 300 mm, mean error within 18 mm at 100 to 800 mm (M4); 17-byte result read 541 µs; reset to first valid reading about 43 ms |

---

## 3. Hardware Architecture & Circuit Topography

### 3.1 Power Distribution Network (PDN)

The system runs from a **1S Li-ion/LiPo/LiHV cell (3.0 to 4.35 V)**. Rails are split to protect the logic from motor transients.

* **High-current rail (raw battery):** feeds the positive terminal of all four motors directly.
* **Low-noise logic rail (3.3 V):** from the **AP2112K-3.3** (600 mA, dropout about 250 mV at full load).
  * **Dropout implication:** at full load the rail regulates only above about 3.55 V input. Under motor load a 1S cell can sag below this, which collapses the 3.3 V rail and causes spurious resets. Mitigations are listed below and tracked in §6.
  * **Regulator input and output:** each carries a **10 µF MLCC in parallel with a 100 nF ceramic**, placed at the regulator pins. Ceramics only: tantalum is avoided on battery-connected rails because inrush can cause short-circuit failures, and the AP2112K is stable with ceramic output capacitors.
  * **Bulk capacitance:** a 220 to 470 µF low-ESR polymer capacitor at the battery star point, added at the PCB stage.
* **VBAT sense:** 100 kΩ / 100 kΩ divider to **GPIO 10 (ADC1_CH9)** with 100 nF at the pin. ADC1 only, because ADC2 conflicts with Wi-Fi. Two-point calibrate against the multimeter. The firmware low-voltage cutoff and brownout-threshold tuning built on this input are scheduled in §6.1.
* **Brownout detector:** the ESP-IDF default detector stays enabled. Its threshold is tuned by measurement once motors run under load.
* **Reverse-polarity protection:** footprint for a P-channel MOSFET on the PCB (Phase 2).
* **Bench power rules (18650):**
  * Use a **protected, high-drain cell** (roughly ≥ 10 A continuous, in the 25R/30Q/VTC class). Never a salvaged cell of unknown condition.
  * **Inline fuse of 5 to 7.5 A** on the first tests. A wiring error without a current-limited supply is a fire risk, not just a dead MOSFET.
  * An 18650 weighs about 45 g and is **likely too heavy for flight**. The flight pack is chosen in Phase 4 from the thrust data.
  * Charge only on a proper charger, off the aircraft, attended.

### 3.2 Actuation Subsystem (Brushed ESC Shield)

Each motor has an independent low-side switch:

* **Switch:** **AO3400A** N-channel MOSFET (SOT-23). Verify against the datasheet: Rds(on) is of the order of 33 mΩ at 4.5 V and 48 mΩ at 2.5 V, so 3.3 V logic drive is adequate. Continuous Id is about 5.7 A.
* **Gate network:** **47 Ω series** resistor (placed at the gate) plus a **10 kΩ gate-to-ground** pull-down. The pull-down holds the MOSFET off while the ESP32-S3 is booting, flashing or crashed. The series resistor damps ringing at 20 to 32 kHz.
* **Flyback diode:** **SS34** (3 A) across each motor (cathode to battery positive, anode to MOSFET drain). Freewheel current is approximately $I_{motor}(1-D)$, which exceeds 1 A at low duty and a few amps of motor current.
* **EMI suppression:** 100 nF ceramic across the motor pads.
* **PWM:** MCPWM at **20 kHz initially** (inaudible), with 32 kHz as an option. One MCPWM timer drives all four outputs, so the period is common. With a 40 MHz timer resolution:
  $$N_{counts} = \frac{f_{res}}{f_{pwm}} = \frac{40\,\text{MHz}}{20\,\text{kHz}} = 2000 \;(\approx 11\text{ bit}), \qquad \frac{40\,\text{MHz}}{32\,\text{kHz}} = 1250 \;(\approx 10.3\text{ bit})$$
  **M7a (ESP-IDF 5.5.3):** clock PLL_F160M / 2 / 2 = 40 MHz, 2000 ticks per period, compare values updated at counter zero, prescalers 1 to 256 and a 16-bit peak per `mcpwm_ll.h`. Duty 0 is held by the force below, not by a compare value of 0. The check against the Technical Reference Manual (MCPWM chapter) is still open.
* **Hardware cutoff:** the MCPWM generators support forcing the output level. The emergency routine uses this to pull gates low independently of the duty registers (§4.8). **M7a:** `mcpwm_generator_set_force_level()` is not IRAM-placed in 5.5.3 and logs on error, so the force-low and release paths write the continuous force (immediate update) through `hal/mcpwm_ll.h` from IRAM, with interrupts masked for the shared register's read-modify-write. The operator and generator IDs are verified by register read-back at init, and all four outputs are forced low before the timer starts. Bench: force-low written 1.48 µs after a cut decision.

### 3.3 Sensor Bus Topology (two buses)

| Bus | Controller | Device | Address | Speed |
|---|---|---|---|---|
| **A** | I2C0 | MPU-6500 | 0x68 (AD0 low) | 400 kHz (device maximum) |
| **B** | I2C1 | VL53L1X | 0x29 (default) | 400 kHz |

* The IMU has its own bus so that a ToF transaction can never delay an IMU read, which removes a source of Core 1 jitter.
* Pull-ups per §2.2.
* **Layout constraint:** the MPU-6500 sits at the geometric centre of the board, isolated from MOSFET and motor traces. I²C traces run over a solid ground plane to prevent inductive coupling from the motor paths.
* **Mechanical constraint:** soft-mount the IMU (foam tape, silicone grommets, or a TPU mount). Hard-mounting a MEMS gyro to a vibrating frame is the largest noise source in small brushed quads.
* **XSHUT** is driven open-drain style: drive low to reset, release to high-impedance to run. This avoids driving 3.3 V into a part that may run its I/O at 2.8 V.

### 3.4 Microcontroller Pin Mapping (ESP32-S3-N16R8)

| Function | GPIO | Type | Note |
|---|---|---|---|
| Motor 1 (Front Left) | 4 | PWM out | MCPWM |
| Motor 2 (Front Right) | 5 | PWM out | MCPWM |
| Motor 3 (Rear Left) | 6 | PWM out | MCPWM |
| Motor 4 (Rear Right) | 7 | PWM out | MCPWM |
| IMU I2C SDA (Bus A) | 1 | Open-drain | I2C0 |
| IMU I2C SCL (Bus A) | 2 | Open-drain | I2C0 |
| MPU-6500 INT | 9 | Input, rising edge | Data-ready ISR, allocated on Core 1 |
| ToF XSHUT | 8 | Open-drain | Reset and init |
| VBAT sense | 10 | ADC1_CH9 | Divider 100k/100k |
| ToF I2C SDA (Bus B) | 11 | Open-drain | I2C1 |
| ToF I2C SCL (Bus B) | 12 | Open-drain | I2C1 |
| ToF GPIO1 (optional) | 13 | Input | Ranging-ready interrupt |
| Spare (status LED, buzzer) | 14, 15 | Free | |

**Avoid:**

| GPIOs | Reason |
|---|---|
| 0, 3, 45, 46 | Strapping pins |
| 19, 20 | Native USB (CDC, and built-in JTAG debug) |
| 26 to 32 | SPI flash |
| 33 to 37 | Octal PSRAM (N16R8) |
| 43, 44 | UART0 |
| 38, 48 | Possible onboard LED. Reserved until identified (§6.1) |

**Free spares:** 16, 17, 18, 21, 39 to 42, 47. (39 to 42 are the classic JTAG pins and are free because debugging uses the built-in USB JTAG.)

### 3.5 Programming, boot and debug path

**Port.** A USB-C receptacle that brings out **D+ and D−** (not the power-only 6-pin part used on the P2-0 test board), wired to the ESP32-S3 native USB Serial/JTAG: **GPIO 19 = D−, GPIO 20 = D+** (*verify against the ESP32-S3 datasheet*). It carries flashing, the log, the 1 kHz telemetry and JTAG debug, exactly as on the dev board in Phase 1. No USB-UART bridge is needed.

**Circuit (details fixed in P2-5 from the Espressif hardware design guidelines and the module datasheet):**

| Item                                                                                                                             | Purpose                                             |
| -------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------- |
| 5.1 kΩ from CC1 and from CC2 to GND                                                                                              | Lets a USB-C host supply 5 V (as on the test board) |
| USB ESD protection array on D+ and D−                                                                                            | Protects GPIO 19 and 20                             |
| Series resistors on D+ and D− only if the guidelines require them                                                                | *Verify*                                            |
| EN pin: pull-up and capacitor to GND (module datasheet recommends an RC, typically 10 kΩ and 1 µF, *verify*) plus a reset button | Clean power-up and manual reset                     |
| BOOT button from GPIO 0 to GND (GPIO 0 is a strapping pin, §3.4)                                                                 | Manual download mode                                |
| UART0 pads: TX GPIO 43, RX GPIO 44, GND, 3V3                                                                                     | Fallback log and flashing path                      |
| Test points: 3V3, VBAT, GND, each motor gate                                                                                     | Bring-up                                            |

**Normal flashing.** `pio run -t upload` over USB. The ROM bootloader on the native port normally resets and enters download mode without the buttons. If the board does not enumerate (blank or crashed firmware), hold BOOT, tap EN, release BOOT: it enumerates as `303a:1001`. Flash mode is re-verified on the first boot of the bare module, because the Phase 1 DIO-at-80-MHz setting came from the dev board's flash chip.

**Power path (open, D-P2-08).** Constraints:
1. VBUS (5 V) must never reach the motor rail, and must never back-feed the battery.
2. The battery must not be forced through a series diode on the logic path: the AP2112K already needs about 3.55 V in at full load (§3.1), and a Schottky drop would shrink the usable battery range further.
3. Phase 3 needs USB data **and** battery-powered motors at the same time.
4. A 5 V input into the AP2112K dissipates about $(5 - 3.3)\,\mathrm{V} \times I$ in a small package next to the IMU. At 300 mA that is about 0.5 W (estimate; thermal limit *verify against the datasheet*).

Options for P2-2: **(A)** ideal-diode OR-ing of USB and battery into the LDO input; **(B)** a jumper or header that selects one source, never both, with USB data still connected when the battery powers the board (VBUS left off the board's power path by a link); **(C)** USB powers the logic only through an open-by-default jumper for bench flashing. Default to B in Rev A unless A fits the mass and area budget.

**First power-up plan (written in P2-9):** USB only, battery and motors disconnected, props off. Check 3V3, the current draw and enumeration. Then flash the Phase 1 firmware and run its self-tests. Battery only after that: protected high-drain cell behind the 5 to 7.5 A inline fuse, attended.

---

## 4. Software Architecture & Data Flow

### 4.1 Stack & Toolchain

* **Build:** VS Code + PlatformIO on Ubuntu, targeting ESP32-S3 (N16R8), using the official `espressif32` platform with **`framework = espidf`** (pure ESP-IDF 5.x). Arduino is not used.
* **ESP-IDF only:** application entry is `app_main()`. All drivers (I²C master, MCPWM, GPIO ISR, GPTimer, ADC) are written directly against **ESP-IDF 5.x APIs**. Every module introduces the IDF API it uses, with the relevant documentation and Technical Reference Manual references, so the project doubles as an ESP-IDF learning path.
* **Platform pinning:** `platformio.ini` pins `platform = espressif32 @ 6.13.0`, which ships `framework-espidf` 5.5.3 (ESP-IDF 5.5.3). The platform is never left unpinned: later releases (7.x and 8.x) move to ESP-IDF 6.x, which renames options and splits components, and would need a blueprint diff. Component dependencies use the split names (`esp_driver_gpio`, `esp_driver_rmt`, `esp_driver_i2c`, `esp_driver_mcpwm`, `esp_driver_gptimer`, `esp_adc`), never the legacy `driver` catch-all. Every `sdkconfig.defaults` name is verified against the generated config.
* **Host tools:** Python 3 virtual environment with `numpy`, `scipy`, `matplotlib`, `pyserial` and `websockets` for logging, latency and FFT work.
* **Linux setup:** user in the `dialout` group, and a udev rule for the Espressif USB device (VID `303a`).
* **Logging:** USB Serial/JTAG (native USB) for bench work. Baud rate is irrelevant on native USB, which runs at full-speed USB. The same binary frames go out over UDP once USB is disconnected.

### 4.2 Core and Task Map

```
 [ CORE 0: Network & Telemetry ]                [ CORE 1: Real-Time Flight Control ]
+--------------------------------------+       +---------------------------------------+
| Wi-Fi AP (IDF internal, pinned)      |       | flight_task  (prio MAX-2, rule 1)     |
| HTTP + WebSocket server (LittleFS)   |       |   woken by MPU-6500 INT ISR           |
| udp_rx_task        (prio ~5)         |       |   1. I2C0 burst read (14 B)           |
| tof_task           (prio ~6, I2C1)   |       |   2. Scaling, notch chain             |
| battery_task       (ADC1, 100 Hz)    |       |   3. Mahony attitude (+ altitude KF)  |
| telemetry_task     (ring-buffer drain)|      |   4. Angle P loop -> rate PID loop    |
|                                      |       |   5. Mixer, battery comp, MCPWM write |
|     lock-free hand-off (SPSC / seqlock)      |   6. Heartbeat + telemetry enqueue    |
|        setpoints ----------------------------> setpoint slots (seqlock, per transport)|
|        <---------------------- telemetry ring (internal SRAM)                        |
|        <---------------------- ToF slot (seqlock)                                    |
+--------------------------------------+       +---------------------------------------+
  GPTimer watchdog ISR (IRAM, cache-safe; Core 1, level 3, every 500 us; enforced only while ARMED):
  heartbeat > 5 ms or data-ready > 3 ms -> gates forced low and the hard latch set (M7b)
```

**Rules for Core 1**

1. **Single task, single pass.** The whole pipeline runs inside `flight_task`. Splitting it across tasks adds scheduling latency for no benefit. It runs at `configMAX_PRIORITIES - 2`, the highest priority available: the IDF IPC task holds the top priority on each core (M7a).
2. **Interrupt allocation.** `gpio_install_isr_service()` and the INT handler are installed **from a task pinned to Core 1**, because the interrupt is allocated on the core that registers it. This keeps it off Wi-Fi's core. The ISR (`IRAM_ATTR`) only calls `vTaskNotifyGiveFromISR()`. **M7b:** Core 1 runs only `flight_task`, IDLE1 and ipc1. Its interrupts are the data-ready GPIO, I²C0, the GPTimer watchdog (level 3), the tick and the cross-core interrupt; the interrupt watchdog uses interrupt 24 (level 4). The FreeRTOS timer task ("Tmr Svc") is unpinned in the IDF 5.5.3 defaults and is pinned to Core 0 (`CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0=y`). The bench firmware prints the task table and `esp_intr_dump()` at every boot. **M7c:** the Wi-Fi PHY calibration, about 0.79 s after start, holds off the Core 1 watchdog interrupt for 513 to 615 µs once per boot, while the machine is still in BOOT or SAFE (never ARMED). Accepted, no change.
3. **No heap allocation after initialisation.** All buffers are static. Monitor `heap_caps_get_free_size()` and the minimum free heap from Core 0.
4. **No `ESP_LOG`/`printf`** in the loop. Use the binary telemetry ring only. **M7a:** the flight, safety and hal_mcpwm code is placed in IRAM through component linker fragments, and its archives reference no logging, printf, heap or atomic library calls (checked with `nm`). IDF exceptions: the I²C master transfer (in flash) logs `ESP_LOGE` on an I²C timeout, reached only on a bus fault; `i2c_master_bus_reset()` logs on failure, reached only by the IMU recovery while not ARMED (M7b); and `abort()` prints before panicking, reached only by the bench fault key `X` (M7b). M7b extends the IRAM placement and the `nm` check to the `wdt` and `hal_gptimer` components.
5. **No PSRAM** for flight data or code. Control code is in IRAM or internal flash cache and control data in internal DRAM.
6. **No flash writes (LittleFS, NVS) while armed.** Flash operations disable the cache and can stall IRAM-unsafe code. Firmware write paths refuse to run in the `ARMED` state.
7. **Yield properly.** `flight_task` blocks on its notification each cycle, so IDLE1 runs and the task watchdog stays satisfied.
8. **Hand-offs are lock-free.** SPSC ring for telemetry, seqlock slot for ToF, and for setpoints **one seqlock slot per transport** (UDP task, httpd task), because a seqlock has a single writer: each record carries its receive time (`esp_timer_get_time()`) and a check word, and Core 1 uses the newer of the two (M6). The seqlock reader copies the buffer holding the last complete value even while a write is in progress (write n fills buffer n & 1, never the one being read) and retries only if the writer laps it, so a 50 Hz writer cannot make a 1 kHz reader give up (M7a: 0 give-ups over 13 964 UDP and 10 000 WebSocket writes; the earlier reader, which retried whenever a write was in progress, gave up about once per 10 s, never with a torn record). The Core 0 writers do not use a critical section (M7b decision D3, `board::kSetpointCriticalSection = false`). A/B over 10 000 frames per transport: 0 give-ups and 0 torn reads on Core 1 with and without it; with it, each write cost 3 to 14 µs more on average and up to 3.9 ms in total under load. Without it, a preempted writer leaves the slot mid-write for up to 1.4 ms, and Core 1 reads the previous complete value meanwhile (one frame old). Never share a user-level mutex, semaphore or queue with Core 0. The FreeRTOS kernel spinlock taken by task notifications is unavoidable; its effect on wake-up latency is measured in M3 (Wi-Fi off) and M7 (Wi-Fi on).
9. **No I2C bus recovery in the flight loop.** A stalled IMU is handled by the missing-data-ready cut trigger (§4.8). Bus recovery by tear-down and rebuild allocates memory, so it is allowed only while DISARMED, never from the flight loop. Investigate `i2c_master_bus_reset()` in M3 as an allocation-free option. Investigated in M3 (ESP-IDF 5.5.3): allocation-free; takes the shared periph_spinlock briefly; on the ESP32-S3 the driver does not wait for the 9-pulse bus clear to finish, so the clear may be cut short; callable only by the task that owns the bus, between transfers. Usable for recovery while DISARMED only. **M7b recovery (replaces the tear-down and rebuild):** when data-ready has been missing for 50 ms in SAFE or DISARMED, the flight task runs `i2c_master_bus_reset()` and the IMU init (about 104 ms), up to 3 attempts per episode, 1 s apart; never while ARMED (the watchdog cuts first). On success the machine returns to SAFE for a new gyro calibration. When all attempts fail, Core 0 makes a controlled restart, at most 2 per power-on (counted in RTC memory), then holds BOOT with the gates low and reports. Flight firmware never rebuilds the bus. Bench: 67 recoveries, all successful (104 ms), none while ARMED; with the IMU disconnected, 3 failed attempts led to controlled restart 1/2 and a boot that stayed in BOOT with the gates low (`FLIGHT: FAIL`).

### 4.3 Timing Budget (1 kHz, 1000 µs frame)

| Step | Estimate | Note |
|---|---|---|
| I²C burst read, 14 B at 400 kHz | 450 µs measured (M2 soak: min 450, avg 450, max 464 µs; wire time about 390 µs plus about 60 µs driver) | $\approx 160\text{ clocks} / 400\text{ kHz}$. A 6-byte gyro-only read is about 260 µs (estimate: 81 clocks, about 200 µs wire time, plus about 60 µs driver overhead; measure before relying on it). **Under Wi-Fi page-serving load (M5):** average 605 to 633 µs, maximum 767 µs (idle AP: 451 µs). **Under latency traffic (M6, 100 frames/s):** UDP 470 to 480 µs, WebSocket 514 to 534 µs average, maximum 778 µs; the M6 setpoint read on Core 1 (from flash) cost 3 µs idle, 8 µs under UDP and 22 µs under WebSocket on average, 97 µs maximum. The margin must be re-checked once the estimator, PID and mixer are measured (R5) **M7c, IDF I²C master transfer in IRAM (default since M7c, B-M7c-11):** under page serving 479 µs average, 540 µs maximum, against 620/775 µs from flash; idle unchanged at about 453 µs. The 6-byte gyro read measured 433 µs average under page serving, not the 260 µs estimate |
| Filters, attitude, PID, mixer | < 150 µs | ESP32-S3 at 240 MHz with single-precision FPU. **To be measured.** M7c stand-in: a 150 µs busy-wait after each IMU read (the synthetic load); replaced by the measured cost once the estimator exists |
| MCPWM update plus telemetry enqueue | < 50 µs | **To be measured** with telemetry. M7a (no estimator, PID or telemetry): setpoint read, safety step and MCPWM write add about 5 µs to the burst read (loop work 455.7 µs average against a 451.2 µs burst, idle AP) |
| ISR-to-task wake-up | 7 µs average, 13 µs maximum measured | M3, Wi-Fi off. M5, Wi-Fi on: 7.0/17 µs average/maximum with the AP idle, 7.9/22 µs under page-serving load. M7a, flight task with Wi-Fi traffic: maximum 15 µs under UDP and 17 µs under WebSocket. M7c, full task layout: maximum 20 to 21 µs under idle, UDP, WebSocket and page serving, 22 µs in the 20-minute soak |
| **Margin** | **Average margin ≥ 350 µs; worst-frame margin ≥ 250 µs** | Measured 297 µs at the worst frame with the I²C transfer in IRAM and the 150 µs stand-in load; re-measure when the real estimator, PID and mixer exist; the 32 KB instruction cache (about 37 µs more, 16 KB RAM) stays in reserve. Target loop-period jitter p99.9 < 100 µs. M7c, worst frame under page serving with the 150 µs load: loop work maximum 929 µs with the transfer from flash (71 µs left), 703 µs with it in IRAM (297 µs left). Loop-period jitter (\|wake-to-wake − 1000 µs\|) p99.9 4 to 12 µs per condition, 9 µs with the load |

### 4.4 Signal Chain

1. **MPU-6500 configuration** (registers verified in docs/mpu6500_notes.md, which carries the page references):
   * `PWR_MGMT_1` (0x6B): init sequence. Write 0x80 (`DEVICE_RESET`), wait at least 100 ms, confirm `PWR_MGMT_1` reads 0x01 (`SLEEP` = 0, `CLKSEL` = 1: auto-select, PLL if ready), then configure all other registers. The IMU is not power-cycled when the ESP32 resets, so it may still hold its previous configuration. Never write 0x00 to `PWR_MGMT_1` (it selects the internal oscillator, ±10 % over temperature). Allow 35 ms after waking before trusting the gyro.
   * `SMPLRT_DIV` (0x19) = 0 gives 1 kHz only when `FCHOICE_B` = 00 and `DLPF_CFG` = 1 to 6. It is ignored for `DLPF_CFG` 0 or 7 (8 kHz) and when the DLPF is bypassed (32 kHz).
   * `CONFIG` (0x1A): `DLPF_CFG` as a compile-time constant (default 1: 184 Hz gyro bandwidth, 2.9 ms delay, about 3.5 ms total gyro delay including the I²C read and processing). `DLPF_CFG` = 2 is 92 Hz and 3.9 ms. The 188 Hz/1.9 ms and 98 Hz/2.8 ms figures are the temperature-sensor column of the datasheet table, not the gyro. The alternative `DLPF_CFG` = 0 is described in item 2.
   * `GYRO_CONFIG` (0x1B) = 0x10: ±1000 °/s, 32.8 LSB/(°/s), `FCHOICE_B[1:0]` = 00. Any other `FCHOICE_B` value bypasses the DLPF and `SMPLRT_DIV`.
   * `ACCEL_CONFIG` (0x1C): ±8 g, 4096 LSB/g.
   * `ACCEL_CONFIG2` (0x1D) = 0x03: accelerometer DLPF 41 Hz, 11.8 ms, 1 kHz. The reset value is 460 Hz. The MPU-6500 filters the accelerometer separately from the gyro.
   * `INT_PIN_CFG` (0x37) = 0x10: push-pull, active high, 50 µs pulse, `INT_ANYRD_2CLEAR` = 1 so the 14-byte burst read clears the interrupt status. `INT_ENABLE` (0x38) = 0x01 (bit 0 `RAW_RDY_EN`): data-ready pulse on INT. Write `CONFIG` before enabling the interrupt, because its reset value gives 8 kHz data-ready.
   * `ACCEL_XOUT_H` (0x3B): 14-byte burst (accel, temperature, gyro). `WHO_AM_I` (0x75) reads 0x70.
2. **Interrupt-driven sampling:** the data-ready pulse on GPIO 9 wakes `flight_task`, so the loop runs on the sensor's own clock. The ISR timestamps each pulse and the loop's $\Delta t$ comes from that interrupt timestamp: the sensor clock is accurate to ±1 % (PLL), so $\Delta t$ is measured, not assumed. Two compile-time constants in `board_config` set the sampling: the `DLPF_CFG` value and an ISR decimation factor (1 for `DLPF_CFG` 1 to 6, 8 for `DLPF_CFG` 0).
   * **Option B (default):** `DLPF_CFG` = 1 (184 Hz, 2.9 ms), 1 kHz data-ready, decimation 1, so each iteration consumes exactly one sample. About 3.5 ms total gyro delay.
   * **Option A (to be evaluated):** `DLPF_CFG` = 0 (250 Hz, 0.97 ms), 8 kHz data-ready (`SMPLRT_DIV` is ignored), decimation 8, so `flight_task` acts on every 8th pulse. About 1.5 ms total gyro delay, at the cost of weaker anti-aliasing and an 8 kHz interrupt on Core 1.
   * Measured in M3: `DLPF_CFG` = 1 gives 999.74 Hz data-ready with interval standard deviation 0.44 µs and zero missed samples over 10 minutes; `DLPF_CFG` = 0 gives 7997.9 Hz with no reads, but under read traffic one raw pulse was occasionally lost (raw interval up to 251 µs, read interval up to 1126 µs). Option B is confirmed as the default. Option A stays a Phase 3 step 7 experiment because of the lost-pulse behaviour. The final choice is made in Phase 3 step 7 from measured spectra.
3. **On-chip digital DLPF:** applied at the 8 or 32 kHz internal rate before the output rate is set, it is the only defence against motor-frequency noise above the 500 Hz Nyquist limit of a 1 kHz loop.
4. **Software biquad notch chain.** Notches work only below 500 Hz. With $\omega_0 = 2\pi f_0/f_s$ and $\alpha = \sin\omega_0/(2Q)$:
   $$H(z)=\frac{1-2\cos\omega_0\,z^{-1}+z^{-2}}{(1+\alpha)-2\cos\omega_0\,z^{-1}+(1-\alpha)z^{-2}}$$
   The centre frequencies come from the Phase 3 spectra. A throttle-tracking dynamic notch is a later enhancement.
5. **Attitude estimation.** Mahony filter on quaternion $\hat q$:
   $$e = \hat v \times v_{acc}, \qquad \dot{\hat q} = \tfrac{1}{2}\,\hat q \otimes \begin{bmatrix}0\\ \omega_{gyro} + k_P\,e + k_I\!\int e\,dt\end{bmatrix}$$
   where $v_{acc}$ is the normalised accelerometer vector and $\hat v$ is the gravity direction predicted by $\hat q$. Accelerometer correction is weighted down when $\lvert\lvert a\rvert\rvert$ departs from 1 g.
6. **Altitude estimation.** A linear Kalman filter with state $x=[z,\;v_z,\;b_a]^\top$ (height, vertical velocity, accelerometer bias) and input $u=a_z$ (world-frame vertical acceleration with gravity removed):
   $$x_{k+1}=\begin{bmatrix}1&\Delta t&-\tfrac{\Delta t^2}{2}\\0&1&-\Delta t\\0&0&1\end{bmatrix}x_k+\begin{bmatrix}\tfrac{\Delta t^2}{2}\\ \Delta t\\ 0\end{bmatrix}u_k,\qquad P_{k+1}=FP_kF^\top+Q$$
   Update with the tilt-compensated ToF range $r$:
   $$y = r\cos\phi\cos\theta,\quad H=[1\;0\;0],\quad K=PH^\top(HPH^\top+R)^{-1},\quad x\leftarrow x+K(y-Hx),\quad P\leftarrow(I-KH)P$$
   The dynamics and measurement model are linear in the states (the tilt compensation is applied to the measurement outside the filter), so no linearisation is needed and an *extended* Kalman filter would add cost without benefit. An EKF is reserved for any future measurement with a nonlinear model (for example optical flow). Innovations beyond a gate are rejected, with an exception for persistent consistent readings (a terrain step, §5 Phase 5). The sensor latency (about one timing budget) is compensated.
7. **ToF configuration (VL53L1X):** short distance mode (≤ about 1.3 m, best ambient immunity) at about 40 Hz (20 ms timing budget, 25 ms inter-measurement period). Read in `tof_task` on I2C1. Driver ported from the Pololu vl53l1x-arduino library (BSD-3-Clause, see docs/THIRD_PARTY_LICENSES.md).
   * Measured in M4: 39.68 Hz after the period-register fix (D-M4-08: ST's inter-measurement formula, `OSC_CALIBRATE_VAL & 0x3FF` × period × 1.075; the Pololu formula gave 42.63 Hz, a 23.46 ms period, below budget + 4 ms).
   * A reading is valid only with the "range valid" status and a range of at least 40 mm, the minimum guaranteed ranging distance (DS12385 p.21).
   * In an empty room, 3.5 to 5 % of readings were still "range valid" (M4 P3). The altitude filter must therefore gate on status and on consistency with neighbouring readings (R7).

### 4.5 Control Laws

**Cascade (angle mode):**
$$\omega_{sp}=K_{p,\theta}\,(\theta_{sp}-\hat\theta),\qquad u_\omega = K_p\,e_\omega + K_i\!\sum e_\omega\,\Delta t - K_d\,\frac{\omega_f[k]-\omega_f[k-1]}{\Delta t},\qquad e_\omega=\omega_{sp}-\omega_f$$
Derivative acts on the **filtered measurement** (not the error), and the integrator is clamped (anti-windup) and frozen when the motors saturate.

**Mixer** (body axes: $x$ forward, $y$ right, $z$ down. Positive roll is right side down, positive pitch is nose up):

| Motor | Thrust | Roll | Pitch | Yaw |
|---|---|---|---|---|
| M1 Front Left | 1 | +1 | +1 | $+\sigma$ |
| M2 Front Right | 1 | −1 | +1 | $-\sigma$ |
| M3 Rear Left | 1 | +1 | −1 | $-\sigma$ |
| M4 Rear Right | 1 | −1 | −1 | $+\sigma$ |

$\sigma = \pm 1$ is set by the physical propeller spin directions (diagonal pairs share direction). It is determined and confirmed on the bench in Phase 3. A wrong sign here makes the drone flip on takeoff.

**Output conditioning**

* **Thrust linearisation:** fit $T_i = a\,d_i + b\,d_i^2$ from the Phase 3 scale test and invert it.
* **Battery compensation:** $d_i' = d_i\,\dfrac{V_{ref}}{V_{bat,f}}$, clamped, so that response does not fade as the cell discharges. Active once VBAT sensing is implemented (§6.1).

**Flight modes:** **Rate** (acro: the stick commands rotation speed, gyro only), then **Angle** (self-level: the stick commands tilt, attitude estimate required), then **Altitude hold** (adds the height estimate and a height loop). Angle mode is proven on the tether before any free flight, because rate mode cannot be flown from a touchscreen.

### 4.6 Communications

* **Wi-Fi AP:** fixed channel (1, 6 or 11), WPA2, **one station maximum**, HT20, `WIFI_PS_NONE` (power-save off), `TCP_NODELAY` on sockets. Wi-Fi IRAM optimisations enabled. Beacon interval 100 TU and `dtim_period = 1`, both set explicitly (a zeroed `wifi_ap_config_t` holds DTIM 0; IDF default 1, verify against IDF docs; set from M7).
* **Ground station (laptop):** runs with Wi-Fi power save off and with background scanning sources (location services, an open Wi-Fi settings panel or menu) off. A scan takes the client off channel for about 1 s (about 4 s for a full scan): M6 measured 50 to 90 ms latency bursts every 15 s and setpoint ages up to 162 ms, past the 150 ms link-silence cut.
* **Joystick UI:** nippleJS served from LittleFS (gzip). Browsers cannot open UDP sockets, so the UI talks **WebSocket** (`/ws`). Left control is a **latching throttle slider** (a self-centring joystick is wrong for throttle). Right is a pitch/roll joystick. Yaw is a slider. Always-visible **ARM** and **KILL** buttons.
  * Sends 50 Hz "latest state only" frames.
  * Page hidden or screen locked means sending stops, which triggers the failsafe.
  * No screen wake lock: browsers offer `navigator.wakeLock` only in a secure context (HTTPS or localhost), not on plain `http://192.168.4.1`. The user sets a long screen timeout.
  * A reconnect **never** auto-arms.
* **HTTP:** static files from LittleFS (gzip), GET only, plus `GET /version`, which returns the firmware version as JSON (M5).
* **WebSocket `/ws`:** binary, final, 14-byte frames only; any other frame, or one that fails validation, closes the session. A plain GET without the upgrade is answered 400. The handler runs in the httpd task on Core 0 and needs `CONFIG_HTTPD_WS_SUPPORT=y`.
* **UDP:** port **5858**. Python test harness and telemetry stream. A native UDP client is also the fallback control path if browser latency misses its target (§6.2, R3); M6 showed it is not needed.

**Control frame (little-endian, 14 bytes):**

| Offset | Field | Type | Note |
|---|---|---|---|
| 0 | magic | u8 | 0xE5 |
| 1 | type | u8 | 1 = control, 2 = ping, 3 = pong |
| 2 | seq | u16 | Wrap-safe sequence number |
| 4 | roll | i16 | −1000 to +1000 |
| 6 | pitch | i16 | −1000 to +1000 |
| 8 | yaw | i16 | −1000 to +1000 |
| 10 | throttle | u16 | 0 to 1000 |
| 12 | flags | u8 | bit0 arm, bit1 kill, bit2 angle mode |
| 13 | crc8 | u8 | Over bytes 0 to 12. CRC-8 polynomial 0x07, init 0x00, no reflection, no final XOR (CRC-8/SMBUS; check value 0xF4 for `"123456789"`). Known-answer vectors: `components/comms/test_vectors/control_frame_vectors.txt`, shared by firmware, host test, Python and the page |

**Ping and pong:** a pong (type 3) echoes the ping's seq, roll, pitch, yaw and flags; its throttle field carries the device turnaround in µs (receive to send, saturating at 65535), so RTT splits into device and network time. Pings never touch the setpoint. The pong carries no device state (no free field); the state is on the serial log until a status frame is decided.

**Latency target (Phase 1, Module 6):** one-way (RTT/2) p95 < 15 ms and p99 < 20 ms over at least 10 000 packets at 50 Hz, measured for both WebSocket and UDP. **M6 result (3 November 2026, ping plus control frame at 50 Hz each):** laptop UDP p95/p99 2.00/2.59 ms, no loss; laptop WebSocket 3.86/5.26 ms, no loss; phone browser WebSocket 4.90/8.25 ms, 0.32 % lost, all in one 1 s phone-side Wi-Fi stall (a 324 ms setpoint gap on the device, which would trip the 150 ms cut).

**No static page serving while ARMED (revision 2.13):** file GETs are answered 503 while the state is ARMED; `/ws` stays open. The UI page is loaded before arming. Reason: the WebSocket and the static pages share one `esp_http_server` task, and continuous page serving delays the WebSocket replies (M7c soak, risk R3). Implemented in the module that builds the joystick UI; control latency is re-tested with the real UI before tether work (Phase 4).

### 4.7 Telemetry & Logging

Binary frame, 1 kHz, about 29 kB/s: sync (2 B), `u32 t_us`, `i16 gx,gy,gz`, `i16 ax,ay,az`, `u16 duty[4]`, `u16 vbat_mV`, crc8. Core 1 writes into an SPSC ring in internal SRAM, and `telemetry_task` on Core 0 drains it to USB or UDP, where a Python script writes CSV for the FFT. **No formatted `printf` text at 1 kHz.** Dropped frames are counted and reported.

### 4.8 Safety, Arming & Failsafe Specification

**State machine:** `BOOT → SAFE → DISARMED → ARMED → (cut) → DISARMED (latched)`. Also (M7b): `SAFE` or `DISARMED → SAFE` after a successful IMU recovery (new gyro calibration), and `SAFE` or `DISARMED → BOOT` (final) when the restart limit is reached; neither is allowed from `ARMED`.

* **BOOT:** before the outputs and the IMU are initialised; gates forced low. A failed output or IMU init keeps the machine in BOOT. **M7b:** before the first transfer the I²C0 lines are always clocked 9 times plus a STOP, and a failed IMU init is retried up to 3 times (bus reset, best-effort DEVICE_RESET, 100 ms wait). Bench: 22 resets while ARMED, each back to DISARMED with the IMU initialised on the first attempt; 3 of the 22 found SDA held low and cleared it.
* **SAFE:** outputs and IMU running, waiting for the gyro calibration and a healthy IMU.
* Clearing the arm flag disarms (not a cut, not latched). The implementation (M7a) is the pure C++ `safety` component, host-tested.

**Arm conditions (all required):**

* Link fresh (< 150 ms old).
* Throttle at minimum (≤ 10 permille) for ≥ 500 ms, and an explicit arm edge (never held across boot or reconnect): the arm flag seen low, then high, in fresh frames after the latest boot, reconnect (stale to fresh, or a transport change) or cut. A refused attempt consumes the edge. KILL set blocks arming.
* Gyro calibration complete with the vehicle at rest: 2000 samples, each axis sd ≤ 0.5 dps and range ≤ 4 dps, \|bias\| ≤ 10 dps; the reference g0 is the mean \|a\| of the same window (0.85 to 1.25 g, R6). At rest: no 100-sample block with \|gyro − bias\| > 3 dps or \|\|a\|/g0 − 1\| > 0.05 for 500 ms.
* Tilt below a limit (provisionally 20°), relative to the body +Z axis.
* VBAT above a minimum (once VBAT sensing is implemented).
* IMU healthy (continuous data-ready).
* Accelerometer fresh (M7c): a value older than 8 ms is stale, and the rest, tilt and accelerometer checks fail. Not a cut while ARMED; the IMU-missing cuts cover a stalled IMU.

**Cut triggers (any one forces all gates low):**

| Trigger | Provisional threshold |
|---|---|
| Control-link silence | **150 ms** |
| Missing IMU data-ready | > 3 ms since the last interrupt timestamp, decided by the GPTimer watchdog ISR every 500 µs, so 3.0 to 3.5 ms plus the ISR entry delay (reason `wd_imu_missing`; M7b bench: 42 cuts, 3017 to 3496 µs, gates low 1.43 to 1.91 µs after the decision). Backup check on the flight task's tick at 6 ms (`imu_missing_tick`) |
| Loop overruns | Counted, with threshold N |
| Filtered VBAT below cutoff | for > 100 ms (once VBAT cutoff is implemented) |
| Tilt beyond crash limit | Configurable. Disabled or relaxed on the tether |
| KILL flag | Immediate |
| `flight_task` heartbeat stall | > 5 ms, decided by the GPTimer watchdog ISR, so 5.0 to 5.5 ms plus the ISR entry delay (reason `wd_heartbeat`; M7b bench: 22 cuts, 5008 to 5465 µs, gates low 1.44 to 1.49 µs after the decision) |
| Watchdog dead | No watchdog alarm for 2 ms: a cut while ARMED and an arm refusal otherwise (`wd_dead`) |

**Cut action:** duty to zero **and** MCPWM generators forced low, then latched. There is **no automatic re-arm**: re-arm needs throttle low plus a fresh explicit arm command. **Hard latch (M7b):** a watchdog cut also sets a hardware-path latch in `hal_mcpwm` (`output_gate.h`, host-tested). It is set only by the watchdog ISR and cleared only when the state machine enters ARMED through the normal arm sequence. While it is set, releasing the generators and writing a duty are refused under interrupt masking, so a stalled flight task that resumes cannot re-enable the outputs.

**Other layers:** task watchdog, brownout detector, 10 kΩ gate pull-downs (cover reset, crash and flashing), and a firmware **throttle cap** for tethered work. **M7b:** interrupt watchdog 100 ms with the CPU1 tick watched; task watchdog 2 s **with panic**, watching IDLE1. **M7c:** IDLE0 is not watched: page serving keeps Core 0 busy for more than 2 s with nothing wrong (23 task-watchdog resets in a loop on the bench), and a reset is itself a hazard in flight. A Core 0 that stops delivering setpoints is caught by the 150 ms link-loss cut on Core 1. Every panic forces the gates low before anything prints: `esp_panic_handler` is wrapped (`-Wl,--wrap`) by an IRAM function that writes two constant MCPWM force registers, then calls the real handler. Bench (3 trials each): interrupts-masked hang (`H`) → `INT_WDT` reset 1.40 to 1.44 s after the hang; starved idle (`T`) → `TASK_WDT` reset 3.21 s; `abort()` (`X`) → `PANIC` reset 1.21 s; each reboot came up DISARMED; GPIO 4 to 7 read 0 V on the meter during every backtrace (0.07 V at throttle 100 before the hang). The time to the panic itself, which is when the gates go low, is not measured directly: `H` exceeds `X` (same print and reboot) by about 0.2 s, so an interrupts-masked hang on Core 1 leaves the outputs driven for about 0.2 s, not the 0.1 s timeout (verify against IDF docs: interrupt watchdog with CPU1 checking). The GPTimer watchdog cannot cover this case, since its level 3 is masked too.

### 4.9 `sdkconfig.defaults` Targets

Names are verified against the pinned IDF version during Module 1 (via `menuconfig`):

* `CONFIG_FREERTOS_HZ=1000`
* `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`, `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`, `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y`
* `CONFIG_ESP_WIFI_IRAM_OPT=y`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=y`
* No runtime flash writes (M5): `CONFIG_ESP_WIFI_NVS_ENABLED=n`, `CONFIG_ESP_PHY_CALIBRATION_AND_DATA_STORAGE=n` (full PHY calibration at every boot), NVS never initialised
* `CONFIG_LITTLEFS_MMAP_PARTITION=y` (M5): LittleFS reads go through the cache-mapped partition, not `esp_flash_read()`
* `CONFIG_HTTPD_WS_SUPPORT=y` (M6): WebSocket support in `esp_http_server` for `/ws`; costs about 6.3 KB of flash and no RAM
* `CONFIG_I2C_ISR_IRAM_SAFE=y`, `CONFIG_GPIO_CTRL_FUNC_IN_IRAM=y`, `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM=y` (M7a; the ISR option, renamed `CONFIG_MCPWM_ISR_CACHE_SAFE` in IDF 5.5.3, is not needed: no MCPWM interrupts)
* `CONFIG_COMPILER_OPTIMIZATION_PERF=y`, 240 MHz CPU (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`)
* Flash mode **DIO at 80 MHz** (`CONFIG_ESPTOOLPY_FLASHMODE_DIO=y`): this board's flash chip is detected as generic and the bootloader cannot set its QIO enable bit
* Brownout detector enabled, with the level chosen by measurement
* Watchdogs (M7b): `CONFIG_ESP_INT_WDT=y`, `CONFIG_ESP_INT_WDT_TIMEOUT_MS=100`, `CONFIG_ESP_INT_WDT_CHECK_CPU1=y`; `CONFIG_ESP_TASK_WDT_INIT=y`, `CONFIG_ESP_TASK_WDT_TIMEOUT_S=2`, `CONFIG_ESP_TASK_WDT_PANIC=y` (off by default in 5.5.3: a timeout only printed), `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n` (M7c), `..._CPU1=y`; `CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT=y`
* GPTimer watchdog (M7b): `CONFIG_GPTIMER_ISR_HANDLER_IN_IRAM=y`, `CONFIG_GPTIMER_ISR_CACHE_SAFE=y` (the interrupt stays enabled while the flash cache is off)
* `CONFIG_FREERTOS_TIMER_TASK_AFFINITY_CPU0=y` (M7b)
* `CONFIG_FE_R5_I2C_IRAM=y` (M7c, R5 (b)): a project Kconfig option (`components/board_config/Kconfig`) that places the IDF `i2c_master` object, `hal_i2c` and `drv_mpu6500` in IRAM through `components/hal_i2c/linker.lf`. IDF 5.5.3 has no option for the synchronous master transfer. Cost 7.4 KB of internal RAM
* Bench-only overlay `sdkconfig.defaults.bench` (environment `esp32s3_bench`): `CONFIG_FREERTOS_USE_TRACE_FACILITY=y` for the task table and `CONFIG_FE_LATENCY_HIST=y` for the M7c latency histograms and synthetic load; never in flight firmware. Further bench-only overlays (M7c): `sdkconfig.defaults.r5b` to `r5e` (R5 experiments, `CONFIG_FE_R5_GYRO_ONLY`, `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`) and `sdkconfig.defaults.heapdiag`, `.nommap` (heap poisoning and isolation)
* 16 MB flash, octal PSRAM, custom partition table (factory app plus a LittleFS partition of about 1 MB)

### 4.10 Repository Layout

```
franken-e58/
├── platformio.ini
├── sdkconfig.defaults
├── partitions.csv
├── CMakeLists.txt
├── src/main.cpp
├── components/
│   ├── hal_i2c/        hal_gpio/    hal_mcpwm/   hal_adc/
│   ├── drv_mpu6500/    drv_vl53l1x/
│   ├── dsp_filters/    estimator/   control/     mixer/
│   ├── safety/         comms/       telemetry/
│   ├── latency/        (M7c: header-only latency histograms, host-tested)
│   └── board_config/   (pin map, constants, limits, bench Kconfig options)
├── data/               (LittleFS: index.html, nipplejs, app.js, gzip)
├── tools/              (latency_test.py, control_client.py, run_m7b.py, analyse_m7b.py, logger.py, fft_analysis.py)
└── docs/               (this blueprint)
```

Phase 2 repository layout (added to normal root directory above):

```
franken-e58/
└── pcb/                          (Phase 2 only; the firmware tree is not touched)
    ├── README.md                 status, stage table, how to open the project
    ├── decision_log.md           every design choice, with date and reason
    ├── mechanical/               photos, measurements.md, constraints.csv,
    │                             SOLIDWORKS model, outline.dxf, cavity.step
    ├── datasheets/               PDFs (git-ignored) and MANIFEST.md
    │                             (source URL, revision, date, SHA-256, page notes)
    ├── specs/                    requirements.md, motor_electrical.md,
    │                             net_table.csv, pin_map.csv, design_rules.md
    ├── reviews/                  one reference-design review per chip
    ├── kicad/
    │   ├── franken-e58/          the KiCad project
    │   └── lib/                  project symbol and footprint libraries
    ├── tools/                    outline check, netlist comparison, BOM and stock check
    └── outputs/                  Gerbers, drill, BOM, pick-and-place, renders, STEP
```

Docs for each stage go in `docs/` as `P2-n_package.md`, as in Phase 1.

---

## 5. Phased Implementation Plan

### Phase 1: Breadboard Prototyping & Telemetry Validation (props off, no motors attached)

**Objective:** validate sensor communication, timing and network performance before any PCB work. Build and prove each module alone.

**Status: complete (9 November 2026, revision 2.14).** M1 to M7c and the integration run passed (table below). Results, decision log, measured timing budget, carried-over items and Phase 2 inputs: `docs/PHASE1_REPORT.md`. Next: Phase 2.

| # | Module | Acceptance criteria |
|---|---|---|
| M1 | PlatformIO skeleton, `platformio.ini`, `sdkconfig.defaults`, USB CDC logging, GPIO 4 to 7 driven low at boot, LED blink probe | Builds and flashes. Timestamped log lines over USB. `menuconfig` names verified. **Onboard LED GPIO identified. Native USB socket identified** (the one that enumerates with VID `303a`) |
| M2 | Dual-bus I²C scan and soak, with bus-recovery routine | MPU at 0x68 and ToF at 0x29 detected. 10-minute soak at 400 kHz with zero NACKs or timeouts. Recovery tested |
| M3 | MPU-6500 driver: config, `DLPF_CFG` as a compile-time constant (default 1: 184 Hz) plus ISR decimation factor, INT-driven read | `WHO_AM_I` = 0x70. INT rate 1000 ± 10 Hz at `DLPF_CFG` = 1 (sensor clock ±1 %). INT rate at `DLPF_CFG` = 0 measured (8000 ± 80 Hz expected if the gyro drives data-ready; the measurement decides, not a pass threshold). Loop $\Delta t$ from the interrupt timestamp. No missed samples in 10 min. Jitter measured |
| M4 | VL53L1X driver in isolation, own bus | About 40 Hz ranging. Status codes handled. Checked against a tape measure at several distances |
| M5 | LittleFS mount, AP bring-up, static file serving | Page served at `http://192.168.4.1` on the fixed channel. Footprint noted. IMU under Wi-Fi (AP idle and under page-serving load): zero missed samples and I²C errors, interval sd < 2 µs, jitter < 20 µs, wake-up max < 50 µs. No runtime flash writes |
| M6 | WebSocket and UDP latency harness | p95 < 15 ms, p99 < 20 ms one-way. Histograms for both transports. Decides whether UDP becomes the primary control path (R3). IMU loop alongside: zero missed samples and I²C errors, interval sd < 2 µs, jitter < 20 µs, wake-up max < 50 µs. Setpoint hand-off: no torn record reaches Core 1. Heap within 2 KB of idle after more than 10 000 frames per transport. **Passed 3 November 2026** |
| M7a | Motor-output layer (MCPWM, forced low), arming and failsafe state machine, link-loss, kill and missing-IMU cuts. No motors connected; bench duty cap 200 permille | Gates low within the 150 ms rule when the link is pulled (≥ 20 trials per transport, decision 150 to 152 ms, force-low < 50 µs). Kill < 2 ms. Missing data-ready > 3000 and ≤ 4100 µs. Every arm refusal reason seen. IMU loop alongside as in M6. **Passed 4 November 2026:** 40 link-loss trials 150.02 to 150.97 ms, force-low 1.48 µs; kill 991 µs; missing data-ready 4031 µs |
| M7b | Safety mechanisms: GPTimer heartbeat and data-ready watchdog with hard latch, interrupt and task watchdogs with panic force-low, task layout and core pinning, IMU recovery and boot retry | Watchdog cuts 5.0 to 5.5 ms (heartbeat) and 3.0 to 3.5 ms (data-ready) plus entry delay, force-low < 50 µs, ≥ 20 trials each. `H`, `T`, `X` reset with the right reason and come up DISARMED. ≥ 20 resets while ARMED, none stuck. Core 1 runs only flight, IDLE1, ipc1. Recovery never while ARMED. 10 minutes ARMED under traffic with no cut or reset. **Passed 7 November 2026:** heartbeat 22 cuts 5008 to 5465 µs; data-ready 42 cuts 3017 to 3496 µs; force-low ≤ 1.91 µs; 22 armed resets all DISARMED; 688 s ARMED with no cut or reset. Details: `docs/M7b_package.md` |
| M7c | Timing under load: latency histograms, 150 µs synthetic load, jitter per condition, R5 experiment builds, 20-minute soak | Loop jitter, defined as \|wake-to-wake − 1000 µs\| over consecutive data wake-ups (1 µs bins), p99.9 < 100 µs, wake-up max < 50 µs, 0 missed, 0 I²C errors, for idle AP, UDP, WebSocket and page serving with the load off; p99.9 < 100 µs under page serving with the 150 µs load, loop-work maximum reported; R5 table and recommendation; 20-minute soak with arm cycles, WebSocket pings and page serving: 0 missed, 0 I²C errors, no unintended cut or reset, heap drift within 2 KB. **Passed 9 November 2026:** jitter p99.9 4/6/7/12 µs, wake-up max ≤ 21 µs; with the load p99.9 9 µs, loop work max 929 µs; R5 (b) adopted; soak 1267 s, 40 arm cycles, 0 missed, heap drift −112 B. Details: `docs/M7c_package.md` |
| P1-INT | Phase 1 integration run: everything together (flight loop, M7 safety, comms, page serving, ToF on bus B, 150 µs load) | ToF 40 ± 2 Hz, valid ≥ 99 %, no no-data records; IMU as in M7c on both buses; no unintended cut or reset; heap drift within 2 KB; with the ToF SDA unplugged for 30 s, IMU and arming unaffected and no-data reported. **Passed 9 November 2026:** I1 (10 min): ToF 39.71 Hz, valid 100 %, period p99.9 12 µs, wake-up max 17 µs, 0 missed, 28 arm cycles, loop work avg/max 644.8/699 µs (average margin 355 µs, worst frame 301 µs). I2: no-data 87 ms after the last reading; readings resumed by themselves after reconnecting, without a re-init; 14 arm cycles unaffected. Bus B interrupt and `tof_task` on Core 0. Details: `docs/P1_integration_package.md` |

### Phase 2: Schematic Capture & PCB Design

**Objective:** one compact, light, 2- or 4-layer **flight-controller board** that carries the ESP32-S3 module, IMU, ToF connection, motor drivers, power stage and USB programming port, and fits the E58 frame cavity. The old shield-board wording is retired (decision D-P2-01).

**Status: P2-0 complete (toolchain), P2-1 next.** Package files: `docs/P2-0_package.md`, `docs/P2-1_package.md`.

**Design decisions so far**

| ID | Decision | Reason | State |
|---|---|---|---|
| D-P2-01 | **Single PCB** with the ESP32-S3 module soldered on. No dev board, no shield | A 44-pin dev board is too tall and heavy for a micro-quad cavity. Mass and height are the main constraints | Decided |
| D-P2-02 | Module: **ESP32-S3-WROOM-1-N16R8** by default | It matches the module family validated in Phase 1 (flash and PSRAM modes, `sdkconfig`). Any other module (N8, MINI) needs the §4.9 options re-verified. Closed in P2-2 (O3) | Default, open |
| D-P2-03 | **USB-C receptacle with D+ and D−** on GPIO 20 and 19 (native USB Serial/JTAG) is the programming, logging and debug port. Plus BOOT and EN buttons and UART0 pads as the fallback (Block 4) | Same native port as Phase 1, no USB-UART bridge needed | Decided in principle. Mass and height checked in P2-1, power path in P2-2 |
| D-P2-04 | **IMU at the geometric centre and centre of gravity**, away from the antenna, LDO, module and motor currents, over solid ground, whole board soft-mounted | Least noise (R1) | Decided. Slot or cut-out option reserved |
| D-P2-05 | **Camera: mechanical only in Rev A** (mount and cable clearance kept, no electrical connection) | The video and Wi-Fi chip is probably on the OEM board, and a DVP camera needs about 12 GPIOs | Pending the P2-1 inspection of the camera connector |
| D-P2-06 | **ToF: breakout on a connector** in Rev A, chip on board later | The VL53L1X is a small optical LGA that needs a cover window | Proposed, confirm in P2-2 |
| D-P2-07 | Assembly method (hand-solder versus fab assembly) | Decides package choices (LGA, QFN, passive size) | **Open**, default fab assembly of SMD parts. Closed in P2-2 |
| D-P2-08 | Power path between USB 5 V and the 1S battery | Phase 3 needs USB logging while the motors run from the battery. VBUS must never back-feed the battery or reach the motor rail | **Open**, closed in P2-2 (Block 4) |

**Steps**

1. **Schematic.** AP2112K with 10 µF MLCC + 100 nF at input and output plus the bulk polymer capacitor; four AO3400A switches with 47 Ω gate resistors and 10 kΩ pull-downs; SS34 flyback diodes; 100 nF motor capacitors; two I²C buses with pull-ups sized per §2.2; VBAT divider; reverse-polarity P-FET footprint; fuse and battery connector; INA219/INA226 footprint (optional); **ESP32-S3 module minimum system, EN reset circuit, BOOT button, USB-C port with CC resistors and ESD protection, UART0 pads, test points and 0 Ω links on every motor line** (B-P2-03).
2. **Placement.** Module and MPU-6500 at the centre. MOSFETs and battery terminals on the periphery. **Keep the Wi-Fi antenna free of copper and ground pours** (module datasheet keep-out, *verify*).
3. **Routing.** Motor supply trace widths **calculated from the measured stall current** (B-P2-08). I²C traces 8 to 10 mil over solid ground. **Star ground** separating motor returns from logic ground.
4. **Module variant and mass.** Close O3. Check total board mass against the OEM board and the flight pack.
5. **Fit check.** Export a STEP from KiCad and check it against the SOLIDWORKS cavity model (P2-8).

Each stage ends at a gate. Nothing moves on until the gate passes and the user signs off. Commit to git at every gate.

| Stage | Work | Gate (acceptance) |
|---|---|---|
| **P2-0** Toolchain | KiCad installed natively, KiCad MCP server connected to Claude Code, JLCPCB parts database, test board | `claude mcp list` shows `kicad` connected. Test board passes ERC and DRC and exports. **Passed 11 November 2026** |
| **P2-1** Mechanical capture | Measure the OEM board and the frame cavity. Camera and connector inspection. Motor stall current. Mass. SOLIDWORKS model and outline DXF. Folder and rules set up | Constraints sheet signed off. Outline imports at the right scale (bounding box within 0.1 mm of the measured value). A 1:1 printout fits the real cavity. Stall current recorded per motor |
| **P2-2** Architecture decision | Close D-P2-02, 05, 06, 07, 08 and O3. Mass budget. IMU placement rules. USB-C height and mass check | User approves the decision log. Blueprint updated |
| **P2-3** Datasheet library | Every IC and part, plus the ESP32-S3 hardware design guidelines and the module datasheet | `MANIFEST.md` complete: URL, revision, date, SHA-256 for every file |
| **P2-4** Specs | Net table (ref, part number, value, footprint, package, supplier number, stock, mass, datasheet), pin map (function, GPIO, module pad), design rules (trace widths and clearances from the fab and the stall current) | Every §3 part is in the net table. Pin map matches §3.4 as revised by B-P2-04. Stock checked |
| **P2-5** Schematic | Reference-design review per chip first (datasheet circuit against ours, every difference listed with a page reference), then the schematic. Footprint land patterns checked against the manufacturer drawings | Every difference justified or fixed. Power budget table (worst-case currents against the LDO rating and the 3.55 V dropout limit) done |
| **P2-6** ERC and netlist check | ERC, netlist export, independent script (not the generator) comparing every pin with the pin map and the datasheet pin tables | ERC clean. 100 % of pins match. Every unused pin has a declared treatment. No unexplained single-pin net |
| **P2-7** Placement | IMU at centre, antenna keep-out, mounting holes and connectors from P2-1, power parts on the periphery | User approves the placement render |
| **P2-8** Routing and DRC | Motor traces, I²C over ground, star ground. DRC. STEP export and fit check in SOLIDWORKS | DRC clean (MCP and `kicad-cli`), no interference with the cavity model |
| **P2-9** Release | Gerbers, drill, BOM, pick-and-place, final stock check, blueprint revision | User signs off. First-power-up plan written (Block 4) |

**Phase 2 rules**

1. **Datasheet first.** No symbol, footprint or net is created from memory. The source (file and page) is recorded. Footprints are checked against the manufacturer land-pattern drawing, not only against the KiCad library.
2. **Every number carries a tag:** `[measured]`, `[datasheet p.N]` or `[assumed]`. Assumed values are listed in the stage report.
3. **Independent verification.** Netlist and pin checks run in a script that does not share code with whatever generated the schematic. ERC and DRC are run through both the MCP server and `kicad-cli`.
4. **Power nets use global power symbols**, never sheet-local labels (the P2-0 test board produced `/GND` against `GND` mismatches).
5. **Git discipline.** Commit before every gate and before any bulk MCP edit. The KiCad GUI is never open on the same file as the MCP server edits.
6. **No autorouting** for the motor and IMU areas.
7. **No guessing.** A missing measurement is asked for, not estimated.

**Blueprint diffs for Phase 2.** Tracked as Block 7 of `docs/Phase2_blueprint_blocks.md` (B-P2-01 to B-P2-10). Each diff is folded into this blueprint when it is accepted at a stage gate; until then the blocks file is the reference for Phase 2 decisions.


### Phase 3: Bench Testing & Noise Profile Calibration

**Objective:** assemble, characterise motors, and map vibration. **Props off** until step 3.

1. Assemble the shield and secure the motors. Check supply rails and gate-pull-down behaviour with a multimeter. Run motors one at a time, props off, protected by the fuse.
2. **Stall current** per motor on the multimeter 10 A range, in series, for one to two seconds only. Optionally add an INA219 for logging.
3. **Thrust stand** (props on, kitchen scale, frame held down): fit the thrust map $T_i(d_i)$, determine $\sigma$ and the propeller directions, and compute the real thrust-to-weight ratio. **Gate:** the VBAT cutoff and brownout-threshold tuning (§6.1) are in place before motors run under load.
4. **Throttle sweep, 0 to 100 %,** while logging at 1 kHz over USB. Compute a Welch-averaged spectrogram (Hann window, 1024 points). Peaks that stay fixed in frequency are **frame resonances**. Peaks that move with throttle are **motor rotation harmonics**.
5. **Wideband capture:** gyro at 8 kHz with `FCHOICE_B` = 00 and `DLPF_CFG` = 7 (3600 Hz bandwidth) via the MPU-6500 FIFO (512 B). Contiguous 3-axis capture at 8 kHz is impossible over I²C (48 kB/s needs more than a 432 kHz SCL), so capture **one axis at a time** (16 kB/s) with `FIFO_MODE` = 1, drained every 10 ms, with overflow detection and nothing else on bus A. Three runs cover X, Y and Z. The FIFO write rate in the 8 kHz mode is not stated in the datasheets, so verify it first by watching `FIFO_COUNT`. Contiguous 3-axis capture would need SPI. This shows what is aliasing into the 1 kHz loop, since coreless-motor rotation is likely well above 500 Hz.
6. Set biquad notch centres and Q in the config. Re-measure. Trial the **soft-mount** and propeller balancing, and compare spectra.
7. Revisit the DLPF setting with the data: Option B (`DLPF_CFG` = 1, 184 Hz) versus Option A (`DLPF_CFG` = 0, 250 Hz, decimation 8) from §4.4 item 2, or a lower bandwidth. The final `DLPF_CFG` choice is made here from the measured spectra.

### Phase 4: Mechanical Mount Engineering & Tethered Tuning

**Objective:** verify stabilisation under controlled constraints.

1. Design and print a downward-facing VL53L1X mount with a clear view of the floor, clear of prop wash and shadow from the frame (about 27° field of view).
2. Build a **single-axis tether rig** (pitch or roll only). Firmware **throttle cap** enabled, prop guards and eye protection in use. **Never tether yaw.**
3. **Tune the rate loop first:** $K_i = K_d = 0$, raise $K_p$ until oscillation starts, back off by about 50 %, add $K_d$ until the oscillation damps, then add a small $K_i$. Verify rate mode on the rig.
4. Add the angle loop ($K_{p,\theta}$). Prove self-levelling against hand disturbances.
5. Select the flight pack (mass, C rating) from the Phase 3 thrust data.

### Phase 5: Untethered Flight Optimisation

**Objective:** drift-free altitude hold and stable free flight.

1. Entry gates: angle mode proven on the tether, failsafe proven on the bench, battery and fuse checked.
2. First free flights in angle mode at low height in an open, netted or obstacle-free area. **No altitude hold yet.**
3. Add altitude hold. The ToF measures **height above the surface directly below**, so a flat object slid under the sensor produces a range step and the drone climbs to hold that height above the new surface. That is expected behaviour, and the filter must handle it: accept the step after $N$ consecutive consistent readings and reset the vertical state rather than rejecting it as an outlier.
4. Tune the altitude Kalman filter's $Q$ and $R$ if slow vertical drift or motor twitching appears in sustained hovers. Fall back to throttle hold when the ToF is invalid.

---

## 6. Open Items, Decisions & Risk Register

### 6.1 Open Items

| ID  | Item                                                                                       | Resolution                                                                                                                                                                                            |
| --- | ------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| O1  | Onboard LED GPIO                                                                           | The M1 probe found no LED on GPIO 38 or 48. One extended probe (47, 21), otherwise closed as not needed: GPIO 14 and 15 are spare for an external status LED. 38 and 48 stay unconnected until closed |
| O2  | Motor stall current and flight pack                                                        | Measured in Phase 3, chosen in Phase 4                                                                                                                                                                |
| O3  | PCB module variant (N16R8 versus N8)                                                       | Decided in Phase 2                                                                                                                                                                                    |
| O4  | Power-protection stage: bulk capacitor, VBAT low-voltage cutoff, brownout-threshold tuning | Scheduled for a later stage. **The VBAT cutoff and brownout tuning must be in place before motors run under load (Phase 3, step 3).** The bulk capacitor is added at the PCB stage                    |
| O5  | USB power path (D-P2-08)                                                                   | P2-2                                                                                                                                                                                                  |
| O6  | Assembly method (D-P2-07)                                                                  | P2-2                                                                                                                                                                                                  |
| O7  | Camera treatment (D-P2-05)                                                                 | Inspect the connector in P2-1                                                                                                                                                                         |
| O8  | OEM motor stall current                                                                    | P2-1 measurement (B-P2-02)                                                                                                                                                                            |

### 6.2 Risk Register

| ID  | Risk                                                                                                                                                                 | Mitigation                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                            |
| --- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R1  | Gyro vibration noise (MPU-6500: gyro noise 0.01 °/s/√Hz, mechanical drive 25 to 29 kHz; vibration sensitivity and lifecycle status are not stated in the datasheets) | Soft-mount, DLPF, accelerometer DLPF (§4.4) and notch filters. If the noise proves unfixable, move to a better IMU                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| R2  | 1S sag collapsing the logic rail                                                                                                                                     | Ceramic capacitors at the regulator, bulk capacitor, VBAT cutoff, brownout detector (O4), and a lighter-draw pack. **Seen on the breadboard (M6, USB power):** the brownout detector reset the board at Wi-Fi start-up (8 times in one session) and once mid-run under WebSocket traffic. The two later runs that day had none; the cause was not isolated. Wi-Fi transmit bursts alone can pull the rail down, so the supply must be sound before M7's Wi-Fi tests. **M7a (4 November 2026):** the first session had I²C bus faults on the IMU bus (one left the IMU without data-ready until reset) and one brownout reset 8 s after the laptop joined the AP; one later boot, after a reset while armed, failed the IMU WHO_AM_I read (0x00) and stayed in BOOT with the gates low. The later sessions ran with zero I²C errors                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| R3  | Browser joystick latency                                                                                                                                             | **Closed by M6:** the phone-browser WebSocket meets the target (p95 4.90 ms, p99 8.25 ms), so it stays the UI control path and UDP serves the tools. Residual: phone-side Wi-Fi stalls (one of about 1 s in 200 s) trip the 150 ms cut; M7's failsafe treats them as link loss **M7c residual:** the WebSocket and the static pages share one `esp_http_server` task. Under continuous page serving (20 requests/s) the WebSocket replies queue behind the page sends (soak: 56 903 pings read, 101 answered within 200 ms). Control frames over WebSocket would be delayed the same way. Proposed for a later module: no page serving while ARMED, or a separate server instance for `/ws` **Rule (revision 2.13):** no static page serving while ARMED: file GETs are answered 503 while ARMED, `/ws` stays open, the UI page is loaded before arming (§4.6). Implemented in the joystick UI module; control latency re-tested with the real UI before tether work (Phase 4)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| R4  | Flash operation stalls: writes, erases and `esp_flash_read()` reads through the SPI flash API disable the cache on both cores                                        | No flash writes while armed (§4.2, rule 6). LittleFS reads go through the cache-mapped partition (`CONFIG_LITTLEFS_MMAP_PARTITION`), so they do not use that API. M5 shows no Core 1 stall while serving pages (wake-up max 22 µs)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| R5  | Kernel spinlock contention delaying ISR-to-task wake-up under Wi-Fi load                                                                                             | Measured in M3 and M7. Fallback: a Core 1 task that waits on an atomic flag set by the ISR instead of a FreeRTOS notification, at the cost of a busy core. **M5 finding:** under page-serving load the wake-up barely moves (max 22 µs against 13 µs), but the I²C burst read grows from 451 to 605–633 µs average (max 767 µs), most likely cache contention on Core 1 code that runs from flash. **M6 finding:** the burst average rises to 470–480 µs under UDP and 514–534 µs under WebSocket traffic, and the flash-resident setpoint read on Core 1 costs 3 µs idle against 22 µs under WebSocket (97 µs maximum): Core 0's larger code footprint under WebSocket (httpd, TCP) evicting Core 1 code from the shared 16 KB instruction cache fits both. Wake-up stays at 19 µs maximum. Candidate fixes in M7, the setpoint read included: the IMU read path in IRAM, a 32 KB instruction cache, and reading the accelerometer at a lower rate than the gyro. The I²C interrupt is already in IRAM (`CONFIG_I2C_ISR_IRAM_SAFE`). **M7a finding:** with the flight, safety and output code (and the setpoint read) in IRAM, the burst average is unchanged from M6 (UDP 456 to 481 µs, WebSocket 514 to 529 µs); the I²C master transfer itself still runs from flash, so the IMU read path remains the candidate **M7c finding (R5 experiments under page serving, load off):** burst average/maximum and loop-work maximum: (a) baseline 620/775 and 783 µs; (b) IDF I²C master transfer in IRAM 479/540 and 553 µs (+7.4 KB RAM); (c) 32 KB instruction cache 499/663 and 669 µs (+16 KB); (d) 6-byte gyro read with the accelerometer at 250 Hz 481 µs average, 773 µs worst frame; (e) b + c 455/509 and 516 µs (+23.3 KB). **(b) adopted as the default** (smallest change meeting the 517.5 µs target, 230 µs off the worst frame). (c) is the reserve if the estimator needs it; (d) does not help the worst frame. Wake-up stayed at 20 to 22 µs maximum |
| R6  | Accelerometer magnitude reads 1.068 g at rest (M3), so correction gated on \|a\| near 1 g would be weakened                                                          | Calibrate before relying on it: normalise by the measured at-rest norm, or a six-position calibration in Phase 3 or 4. Set the Mahony weighting relative to the calibrated norm                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| R7  | ToF reports occasional valid ghost readings in open space                                                                                                            | Altitude filter gates on status and innovation, and accepts a range step only after N consecutive consistent readings (Phase 5 step 3)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| R8  | ToF has no automatic re-initialisation after it stops answering (proposal C2 from M4)                                                                                | Implement before altitude hold (Phase 5): re-init through XSHUT after N silent periods with bounded attempts; the altitude filter falls back to throttle hold meanwhile                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| R9  | First-spin board errors (wrong footprint, pin swap, missing pull-up)                                                                                                 | The P2-3 to P2-6 gates: datasheet review, footprint check, scripted pin-by-pin netlist comparison, test points and motor-line 0 Ω links for bring-up                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| R10 | USB-C connector mass, height or edge position does not fit the cavity                                                                                                | Checked in P2-1 against the cavity measurements. Fallback: UART0 and USB pads with a detachable cable                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| R11 | LDO heating near the IMU, especially on USB power                                                                                                                    | Power budget in P2-5. Keep the LDO and module away from the IMU. Revisit the regulator choice if needed                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |

### 6.3 Architectural Decision Record

| Decision | Rationale |
|---|---|
| Pure ESP-IDF on the official `espressif32` platform | The hybrid `arduino, espidf` build on the official platform pins Arduino 2.0.17 on ESP-IDF 4.4.7, which lacks the 5.x GPTimer, MCPWM and I²C driver APIs this design uses. Pure ESP-IDF gives the 5.x APIs and fits the goal of learning ESP-IDF |
| Ground-up firmware, ESP-Drone as reference only | Keeps the build modular and testable. ESP-Drone is a pure ESP-IDF project, so it reads directly as a reference |
| Two I²C buses | A ToF transaction cannot delay an IMU read |
| 1 kHz loop, INT-driven sampling | Sensor-synchronised loop; $\Delta t$ measured from the interrupt timestamp |
| WebSocket for the browser, UDP for tools | Browsers cannot send UDP. Confirmed by M6: both transports meet the latency target, the phone browser included |
| Mahony attitude plus a 3-state altitude Kalman filter | Linear altitude model, so an extended filter is unnecessary. Cheaper at 1 kHz |
| Ceramic-only regulator capacitors | Tantalum inrush failure risk on a battery-connected rail |
| Console on UART0 plus native USB Serial/JTAG | Both sockets confirmed in M1, with identical log output on either |
| SS34 flyback diodes and 47 Ω gate resistors | Freewheel current margin and gate-ringing control |

---

## 7. Glossary

* **Rate (acro) mode:** the stick commands a rotation speed. Releasing it stops rotation but leaves the drone tilted. Gyro only.
* **Angle (self-level) mode:** the stick commands a tilt. Centring it levels the drone. Needs an attitude estimate.
* **INT (data-ready):** the IMU pulls a pin each time a new sample is ready, so the loop runs in step with the sensor's own clock.
* **`sdkconfig`:** the generated ESP-IDF configuration file (FreeRTOS tick rate, watchdogs, Wi-Fi buffers, brownout level). Overrides live in `sdkconfig.defaults`.
* **Nyquist limit:** half the sample rate (500 Hz at 1 kHz). Vibration above it aliases into lower frequencies and cannot be filtered out in software afterwards.
