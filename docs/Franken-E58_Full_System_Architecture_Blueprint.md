# Franken-E58: Full System Architecture & Implementation Blueprint

**Revision:** 2.2. **Status:** the binding reference for all firmware, hardware and test work on the project. Items under §6.1 (Open Items) are not yet decided and carry stated defaults.

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
| IMU | MPU-6050 on a breakout (silkscreen: VCC) |
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
| MPU-6050 | **Open** SDA→VCC and SCL→VCC | Inconclusive. GY-521 pull-ups usually sit on the *regulated* side of the onboard regulator, so the VCC pin cannot see them. Optional check: power from 3.3 V, then measure DC volts SDA→GND. About 3 V means pull-ups present, about 0 V means none. |

**Pull-up strategy.** Fit **4.7 kΩ external pull-ups to 3V3 on all four lines** on the breadboard. This is safe in every case:

| Bus | On-board | External | Combined |
|---|---|---|---|
| Bus A (IMU) | none or about 4.7 kΩ | 4.7 kΩ | 4.7 kΩ or about 2.35 kΩ |
| Bus B (ToF) | 10 kΩ | 4.7 kΩ | about 3.2 kΩ |

Valid window for 400 kHz Fast-mode at 3.3 V with about 50 pF bus capacitance:

$$t_r \approx 0.8473\,R_p C_b < 300\,\text{ns} \;\Rightarrow\; R_p \lesssim 7\,\text{k}\Omega, \qquad R_p \gtrsim \frac{V_{DD}-0.4\,\text{V}}{3\,\text{mA}} \approx 0.97\,\text{k}\Omega$$

All combinations above sit inside 1 to 7 kΩ. Final PCB pull-up values are chosen in Phase 2 with the same formula.

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
  Verify clock and divider limits against the ESP32-S3 Technical Reference Manual (MCPWM chapter) during the PWM module.
* **Hardware cutoff:** the MCPWM generators support forcing the output level. The emergency routine uses this to pull gates low independently of the duty registers (§4.8).

### 3.3 Sensor Bus Topology (two buses)

| Bus | Controller | Device | Address | Speed |
|---|---|---|---|---|
| **A** | I2C0 | MPU-6050 | 0x68 (AD0 low) | 400 kHz (device maximum) |
| **B** | I2C1 | VL53L1X | 0x29 (default) | 400 kHz |

* The IMU has its own bus so that a ToF transaction can never delay an IMU read, which removes a source of Core 1 jitter.
* Pull-ups per §2.2.
* **Layout constraint:** the MPU-6050 sits at the geometric centre of the board, isolated from MOSFET and motor traces. I²C traces run over a solid ground plane to prevent inductive coupling from the motor paths.
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
| MPU-6050 INT | 9 | Input, rising edge | Data-ready ISR, allocated on Core 1 |
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

---

## 4. Software Architecture & Data Flow

### 4.1 Stack & Toolchain

* **Build:** VS Code + PlatformIO on Ubuntu, targeting ESP32-S3 (N16R8), using the official `espressif32` platform with **`framework = espidf`** (pure ESP-IDF 5.x). Arduino is not used.
* **ESP-IDF only:** application entry is `app_main()`. All drivers (I²C master, MCPWM, GPIO ISR, GPTimer, ADC) are written directly against **ESP-IDF 5.x APIs**. Every module introduces the IDF API it uses, with the relevant documentation and Technical Reference Manual references, so the project doubles as an ESP-IDF learning path.
* **Platform pinning:** Module 1 pins the official `espressif32` platform version in `platformio.ini` after the first successful build. `pio pkg list` reports the `framework-espidf` version (expected ESP-IDF 5.4.x, to be verified), and every `sdkconfig.defaults` name is verified against it.
* **Host tools:** Python 3 virtual environment with `numpy`, `scipy`, `matplotlib`, `pyserial` and `websockets` for logging, latency and FFT work.
* **Linux setup:** user in the `dialout` group, and a udev rule for the Espressif USB device (VID `303a`).
* **Logging:** USB Serial/JTAG (native USB) for bench work. Baud rate is irrelevant on native USB, which runs at full-speed USB. The same binary frames go out over UDP once USB is disconnected.

### 4.2 Core and Task Map

```
 [ CORE 0: Network & Telemetry ]                [ CORE 1: Real-Time Flight Control ]
+--------------------------------------+       +---------------------------------------+
| Wi-Fi AP (IDF internal, pinned)      |       | flight_task  (highest priority)       |
| HTTP + WebSocket server (LittleFS)   |       |   woken by MPU-6050 INT ISR           |
| udp_rx_task        (prio ~5)         |       |   1. I2C0 burst read (14 B)           |
| tof_task           (prio ~6, I2C1)   |       |   2. Scaling, notch chain             |
| battery_task       (ADC1, 100 Hz)    |       |   3. Mahony attitude (+ altitude KF)  |
| telemetry_task     (ring-buffer drain)|      |   4. Angle P loop -> rate PID loop    |
|                                      |       |   5. Mixer, battery comp, MCPWM write |
|     lock-free hand-off (SPSC / seqlock)      |   6. Heartbeat + telemetry enqueue    |
|        setpoints ----------------------------> setpoint slot (atomic, timestamped)   |
|        <---------------------- telemetry ring (internal SRAM)                        |
|        <---------------------- ToF slot (seqlock)                                    |
+--------------------------------------+       +---------------------------------------+
         GPTimer ISR (IRAM, 5 ms): checks flight_task heartbeat, forces gates low on stall
```

**Rules for Core 1**

1. **Single task, single pass.** The whole pipeline runs inside `flight_task`. Splitting it across tasks adds scheduling latency for no benefit.
2. **Interrupt allocation.** `gpio_install_isr_service()` and the INT handler are installed **from a task pinned to Core 1**, because the interrupt is allocated on the core that registers it. This keeps it off Wi-Fi's core. The ISR (`IRAM_ATTR`) only calls `vTaskNotifyGiveFromISR()`.
3. **No heap allocation after initialisation.** All buffers are static. Monitor `heap_caps_get_free_size()` and the minimum free heap from Core 0.
4. **No `ESP_LOG`/`printf`** in the loop. Use the binary telemetry ring only.
5. **No PSRAM** for flight data or code. Control code is in IRAM or internal flash cache and control data in internal DRAM.
6. **No flash writes (LittleFS, NVS) while armed.** Flash operations disable the cache and can stall IRAM-unsafe code. Firmware write paths refuse to run in the `ARMED` state.
7. **Yield properly.** `flight_task` blocks on its notification each cycle, so IDLE1 runs and the task watchdog stays satisfied.
8. **Hand-offs are lock-free.** SPSC ring for telemetry, atomic timestamped slot for setpoints, seqlock slot for ToF. No mutexes shared with Core 0.

### 4.3 Timing Budget (1 kHz, 1000 µs frame)

| Step | Estimate | Note |
|---|---|---|
| I²C burst read, 14 B at 400 kHz | about 400 µs | $\approx 160\text{ clocks} / 400\text{ kHz}$. A 6-byte gyro-only read is about 200 µs |
| Filters, attitude, PID, mixer | < 150 µs | ESP32-S3 at 240 MHz with single-precision FPU. **To be measured** |
| MCPWM update plus telemetry enqueue | < 50 µs | **To be measured** |
| **Margin** | **≥ 400 µs** | Target loop-period jitter p99.9 < 100 µs |

### 4.4 Signal Chain

1. **MPU-6050 configuration** (registers to be verified against the datasheet and register map):
   * `PWR_MGMT_1` (0x6B): wake, clock source = gyro X PLL.
   * `SMPLRT_DIV` (0x19) = 0 gives a 1 kHz output rate when the DLPF is enabled.
   * `CONFIG` (0x1A): `DLPF_CFG = 1` (about 188 Hz gyro bandwidth, about 1.9 ms delay), held as a compile-time constant. Higher delay settings such as 94/98 Hz cost about 2.8 ms, nearly three loop periods. The value is revisited with the Phase 3 data.
   * `GYRO_CONFIG` (0x1B): ±1000 °/s, 32.8 LSB/(°/s).
   * `ACCEL_CONFIG` (0x1C): ±8 g, 4096 LSB/g.
   * `INT_PIN_CFG` (0x37), `INT_ENABLE` (0x38, bit 0 `DATA_RDY_EN`): data-ready pulse on INT.
   * `ACCEL_XOUT_H` (0x3B): 14-byte burst (accel, temperature, gyro). `WHO_AM_I` (0x75) reads 0x68.
2. **Interrupt-driven sampling:** the data-ready pulse on GPIO 9 wakes `flight_task`, so the loop runs on the sensor's own clock and each iteration consumes exactly one sample. This gives clean $\Delta t$ for the PID and filters.
3. **Hardware DLPF:** the sole analogue-domain defence against motor-frequency noise above the 500 Hz Nyquist limit of a 1 kHz loop.
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
7. **ToF configuration (VL53L1X):** short distance mode (≤ about 1.3 m, best ambient immunity) at about 50 Hz (20 ms timing budget). Read in `tof_task` on I2C1. Use ST's ULD API or a vetted open-source port after a **licence check**.

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

* **Wi-Fi AP:** fixed channel (1, 6 or 11), WPA2, **one station maximum**, HT20, `WIFI_PS_NONE` (power-save off), `TCP_NODELAY` on sockets. Wi-Fi IRAM optimisations enabled.
* **Joystick UI:** nippleJS served from LittleFS (gzip). Browsers cannot open UDP sockets, so the UI talks **WebSocket** (`/ws`). Left control is a **latching throttle slider** (a self-centring joystick is wrong for throttle). Right is a pitch/roll joystick. Yaw is a slider. Always-visible **ARM** and **KILL** buttons.
  * Sends 50 Hz "latest state only" frames.
  * Page hidden or screen locked means sending stops, which triggers the failsafe.
  * A reconnect **never** auto-arms.
* **UDP:** Python test harness and telemetry stream. A native UDP client is also the fallback control path if browser latency misses its target (§6.2, R3).

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
| 13 | crc8 | u8 | Over bytes 0 to 12 |

**Latency target (Phase 1, Module 6):** one-way (RTT/2) p95 < 15 ms and p99 < 20 ms over at least 10 000 packets at 50 Hz, measured for both WebSocket and UDP.

### 4.7 Telemetry & Logging

Binary frame, 1 kHz, about 29 kB/s: sync (2 B), `u32 t_us`, `i16 gx,gy,gz`, `i16 ax,ay,az`, `u16 duty[4]`, `u16 vbat_mV`, crc8. Core 1 writes into an SPSC ring in internal SRAM, and `telemetry_task` on Core 0 drains it to USB or UDP, where a Python script writes CSV for the FFT. **No formatted `printf` text at 1 kHz.** Dropped frames are counted and reported.

### 4.8 Safety, Arming & Failsafe Specification

**State machine:** `BOOT → SAFE → DISARMED → ARMED → (cut) → DISARMED (latched)`

**Arm conditions (all required):**

* Link fresh (< 150 ms old).
* Throttle at minimum for ≥ 500 ms, and an explicit arm edge (never held across boot or reconnect).
* Gyro calibration complete with the vehicle at rest ($\lvert\omega\rvert$ below a small threshold).
* Tilt below a limit (provisionally 20°).
* VBAT above a minimum (once VBAT sensing is implemented).
* IMU healthy (continuous data-ready).

**Cut triggers (any one forces all gates low):**

| Trigger | Provisional threshold |
|---|---|
| Control-link silence | **150 ms** |
| Missing IMU data-ready | > 3 ms |
| Loop overruns | Counted, with threshold N |
| Filtered VBAT below cutoff | for > 100 ms (once VBAT cutoff is implemented) |
| Tilt beyond crash limit | Configurable. Disabled or relaxed on the tether |
| KILL flag | Immediate |
| `flight_task` heartbeat stall | > 5 ms, detected by the GPTimer ISR |

**Cut action:** duty to zero **and** MCPWM generators forced low, then latched. There is **no automatic re-arm**: re-arm needs throttle low plus a fresh explicit arm command.

**Other layers:** task watchdog, brownout detector, 10 kΩ gate pull-downs (cover reset, crash and flashing), and a firmware **throttle cap** for tethered work.

### 4.9 `sdkconfig.defaults` Targets

Names are verified against the pinned IDF version during Module 1 (via `menuconfig`):

* `CONFIG_FREERTOS_HZ=1000`
* `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`, `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y`, `CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y`
* `CONFIG_ESP_WIFI_IRAM_OPT=y`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=y`
* `CONFIG_I2C_ISR_IRAM_SAFE=y`, `CONFIG_GPIO_CTRL_FUNC_IN_IRAM=y`, `CONFIG_MCPWM_ISR_IRAM_SAFE=y`, `CONFIG_MCPWM_CTRL_FUNC_IN_IRAM=y`
* `CONFIG_COMPILER_OPTIMIZATION_PERF=y`, 240 MHz CPU
* Brownout detector enabled, with the level chosen by measurement
* Task watchdog enabled, with a timeout tuned in Module 7
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
│   ├── drv_mpu6050/    drv_vl53l1x/
│   ├── dsp_filters/    estimator/   control/     mixer/
│   ├── safety/         comms/       telemetry/
│   └── board_config/   (pin map, constants, limits)
├── data/               (LittleFS: index.html, nipplejs, app.js, gzip)
├── tools/              (latency_test.py, logger.py, fft_analysis.py)
└── docs/               (this blueprint)
```

---

## 5. Phased Implementation Plan

### Phase 1: Breadboard Prototyping & Telemetry Validation (props off, no motors attached)

**Objective:** validate sensor communication, timing and network performance before any PCB work. Build and prove each module alone.

| # | Module | Acceptance criteria |
|---|---|---|
| M1 | PlatformIO skeleton, `platformio.ini`, `sdkconfig.defaults`, USB CDC logging, GPIO 4 to 7 driven low at boot, LED blink probe | Builds and flashes. Timestamped log lines over USB. `menuconfig` names verified. **Onboard LED GPIO identified. Native USB socket identified** (the one that enumerates with VID `303a`) |
| M2 | Dual-bus I²C scan and soak, with bus-recovery routine | MPU at 0x68 and ToF at 0x29 detected. 10-minute soak at 400 kHz with zero NACKs or timeouts. Recovery tested |
| M3 | MPU-6050 driver: config, 188 Hz DLPF, INT-driven read | `WHO_AM_I` = 0x68. INT rate 1000 ± 1 Hz. No missed samples in 10 min. Jitter measured |
| M4 | VL53L1X driver in isolation, own bus | About 50 Hz ranging. Status codes handled. Checked against a tape measure at several distances |
| M5 | LittleFS mount, AP bring-up, static file serving | Page served at `http://192.168.4.1` on the fixed channel. Footprint noted |
| M6 | WebSocket and UDP latency harness | p95 < 15 ms, p99 < 20 ms one-way. Histograms for both transports. Decides whether UDP becomes the primary control path (R3) |
| M7 | Task layout, core pinning, GPTimer watchdog, failsafe state machine | Gates low within the 150 ms rule when the link is pulled. Loop jitter p99.9 < 100 µs. Core 1 never touches Wi-Fi locks |

### Phase 2: Schematic Capture & PCB Design

**Objective:** a compact, light 2- or 4-layer shield board matching the E58 internal footprint.

1. Schematic: AP2112K with 10 µF MLCC + 100 nF at input and output plus the bulk polymer capacitor, four AO3400A switches with 47 Ω gate resistors, 10 kΩ pull-downs, SS34 flyback diodes, 100 nF motor capacitors, two I²C buses with pull-ups sized per §2.2, VBAT divider, reverse-polarity P-FET footprint, fuse and battery connector, INA219/INA226 footprint (optional).
2. Placement: ESP32-S3 module and MPU-6050 at the geometric centre. MOSFETs and battery terminals on the periphery. **Keep the Wi-Fi antenna free of copper and ground pours.**
3. Routing: motor supply traces **40 to 50 mil minimum**, I²C traces 8 to 10 mil over solid ground, **star ground** separating motor returns from logic ground.
4. Decide the module variant for the PCB (WROOM-1 N16R8 versus an N8 variant without PSRAM, which also frees GPIO 33 to 37). Verify total mass against the flight pack.

### Phase 3: Bench Testing & Noise Profile Calibration

**Objective:** assemble, characterise motors, and map vibration. **Props off** until step 3.

1. Assemble the shield and secure the motors. Check supply rails and gate-pull-down behaviour with a multimeter. Run motors one at a time, props off, protected by the fuse.
2. **Stall current** per motor on the multimeter 10 A range, in series, for one to two seconds only. Optionally add an INA219 for logging.
3. **Thrust stand** (props on, kitchen scale, frame held down): fit the thrust map $T_i(d_i)$, determine $\sigma$ and the propeller directions, and compute the real thrust-to-weight ratio. **Gate:** the VBAT cutoff and brownout-threshold tuning (§6.1) are in place before motors run under load.
4. **Throttle sweep, 0 to 100 %,** while logging at 1 kHz over USB. Compute a Welch-averaged spectrogram (Hann window, 1024 points). Peaks that stay fixed in frequency are **frame resonances**. Peaks that move with throttle are **motor rotation harmonics**.
5. **Wideband capture:** gyro with the DLPF disabled at 8 kHz via the MPU-6050 FIFO (1024 B, drained in bursts). This shows what is aliasing into the 1 kHz loop, since coreless-motor rotation is likely well above 500 Hz.
6. Set biquad notch centres and Q in the config. Re-measure. Trial the **soft-mount** and propeller balancing, and compare spectra.
7. Revisit the DLPF setting (188 Hz versus lower) with the data.

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

| ID | Item | Resolution |
|---|---|---|
| O1 | Onboard LED GPIO | Identified by the M1 blink probe. GPIO 38 and 48 stay unconnected until then |
| O2 | Which USB-C socket is the native USB port | Tested in M1 |
| O3 | Motor stall current and flight pack | Measured in Phase 3, chosen in Phase 4 |
| O4 | PCB module variant (N16R8 versus N8) | Decided in Phase 2 |
| O5 | Power-protection stage: bulk capacitor, VBAT low-voltage cutoff, brownout-threshold tuning | Scheduled for a later stage. **The VBAT cutoff and brownout tuning must be in place before motors run under load (Phase 3, step 3).** The bulk capacitor is added at the PCB stage |

### 6.2 Risk Register

| ID | Risk | Mitigation |
|---|---|---|
| R1 | Gyro vibration noise (the MPU-6050 is end-of-life and vibration-sensitive) | Soft-mount, DLPF and notch filters. If the noise proves unfixable, move to a better IMU |
| R2 | 1S sag collapsing the logic rail | Ceramic capacitors at the regulator, bulk capacitor, VBAT cutoff, brownout detector (O5), and a lighter-draw pack |
| R3 | Browser joystick latency | Measured in M6, with native UDP as the fallback control path |
| R4 | Flash write stalls | No flash writes while armed (§4.2, rule 6) |

### 6.3 Architectural Decision Record

| Decision | Rationale |
|---|---|
| Pure ESP-IDF on the official `espressif32` platform | The hybrid `arduino, espidf` build on the official platform pins Arduino 2.0.17 on ESP-IDF 4.4.7, which lacks the 5.x GPTimer, MCPWM and I²C driver APIs this design uses. Pure ESP-IDF gives the 5.x APIs and fits the goal of learning ESP-IDF |
| Ground-up firmware, ESP-Drone as reference only | Keeps the build modular and testable. ESP-Drone is a pure ESP-IDF project, so it reads directly as a reference |
| Two I²C buses | A ToF transaction cannot delay an IMU read |
| 1 kHz loop, INT-driven sampling | Clean $\Delta t$, sensor-synchronised loop |
| WebSocket for the browser, UDP for tools | Browsers cannot send UDP |
| Mahony attitude plus a 3-state altitude Kalman filter | Linear altitude model, so an extended filter is unnecessary. Cheaper at 1 kHz |
| Ceramic-only regulator capacitors | Tantalum inrush failure risk on a battery-connected rail |
| SS34 flyback diodes and 47 Ω gate resistors | Freewheel current margin and gate-ringing control |

---

## 7. Glossary

* **Rate (acro) mode:** the stick commands a rotation speed. Releasing it stops rotation but leaves the drone tilted. Gyro only.
* **Angle (self-level) mode:** the stick commands a tilt. Centring it levels the drone. Needs an attitude estimate.
* **INT (data-ready):** the IMU pulls a pin each time a new sample is ready, so the loop runs in step with the sensor's own clock.
* **`sdkconfig`:** the generated ESP-IDF configuration file (FreeRTOS tick rate, watchdogs, Wi-Fi buffers, brownout level). Overrides live in `sdkconfig.defaults`.
* **Nyquist limit:** half the sample rate (500 Hz at 1 kHz). Vibration above it aliases into lower frequencies and cannot be filtered out in software afterwards.
