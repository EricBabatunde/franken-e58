# MPU-6500 notes for Franken-E58

Sources (both in `docs/datasheets/`):

- **PS**: MPU-6500 Product Specification, PS-MPU-6500A-01, revision 1.3, 05/15/2020 (39 pages).
- **RM**: MPU-6500 Register Map and Descriptions, RM-MPU-6500A-00, revision 2.1, 09/16/2013 (47 pages). The PDF metadata title reads "RS-MPU-6000A-00", but the document itself is the MPU-6500 register map.

Page numbers are the printed page numbers, which match the PDF page numbers in both files. "Not stated" means neither document says it; nothing here is filled in from memory.

All of this applies only if the fitted IMU really is an MPU-6500. The M2 bench run settles that: WHO_AM_I reads 0x70 on an MPU-6500 and 0x68 on an MPU-6050.

---

## 1. Identity and I²C address

| Item | Value | Ref |
|---|---|---|
| WHO_AM_I (0x75) | **0x70** (reset value, read-only) | RM p.44, RM p.9 |
| I²C address, AD0 = 0 | 0b1101000 = **0x68** | PS p.10, PS p.28 |
| I²C address, AD0 = 1 | 0b1101001 = 0x69 | PS p.10, PS p.28 |
| WHO_AM_I differs from the I²C address | Yes: "This is different from the I2C address of the device" | RM p.44 |
| MPU-6050 compatible mode | Exists, but needs a trim change by InvenSense. Normal parts run in MPU-6500 mode. The WHO_AM_I value in compatible mode is not stated | RM p.6 |

## 2. Registers an IMU driver needs

**Reset values.** All registers reset to 0x00 except PWR_MGMT_1 (0x01) and WHO_AM_I (0x70). The self-test registers hold factory values (RM p.9). "The device will come up in active mode upon power-up" (RM p.10).

| Addr | Name | Bits | Reset | Ref |
|---|---|---|---|---|
| 0x19 | SMPLRT_DIV | [7:0] SMPLRT_DIV. SAMPLE_RATE = 1 kHz / (1 + SMPLRT_DIV). **Only effective when FCHOICE_B = 0b00 and 0 < DLPF_CFG < 7** | 0x00 | RM p.12 |
| 0x1A | CONFIG | [7] reserved; [6] FIFO_MODE (1 = drop new data when full, 0 = overwrite oldest); [5:3] EXT_SYNC_SET; [2:0] DLPF_CFG (gyro and temperature DLPF, table in §3.1) | 0x00 | RM p.13 |
| 0x1B | GYRO_CONFIG | [7:5] XG_ST/YG_ST/ZG_ST; [4:3] GYRO_FS_SEL (00 = ±250, 01 = ±500, 10 = ±1000, 11 = ±2000 °/s); [2] reserved; **[1:0] FCHOICE_B** (00 = DLPF in use; any other value bypasses it, see §3.1) | 0x00 | RM p.14 |
| 0x1C | ACCEL_CONFIG | [7:5] XA_ST/YA_ST/ZA_ST; [4:3] ACCEL_FS_SEL (00 = ±2, 01 = ±4, 10 = ±8, 11 = ±16 g); [2:0] reserved | 0x00 | RM p.14–15 |
| 0x1D | ACCEL_CONFIG2 | [7:4] reserved; [3] ACCEL_FCHOICE_B (1 = bypass the accel DLPF); [2:0] A_DLPF_CFG (table in §3.2). **Not present on the MPU-6050** | 0x00 | RM p.15, RM p.6 |
| 0x23 | FIFO_EN | [7] TEMP_OUT; [6] GYRO_XOUT; [5] GYRO_YOUT; [4] GYRO_ZOUT; [3] ACCEL (all 6 bytes); [2:0] SLV_2..SLV_0 | 0x00 | RM p.18 |
| 0x37 | INT_PIN_CFG | [7] ACTL (1 = active low); [6] OPEN (1 = open drain, 0 = push-pull); [5] LATCH_INT_EN (1 = held until status cleared, 0 = 50 µs pulse); [4] INT_ANYRD_2CLEAR (1 = any read clears status, 0 = only an INT_STATUS read clears it); [3] ACTL_FSYNC; [2] FSYNC_INT_MODE_EN; [1] BYPASS_EN; [0] reserved | 0x00 | RM p.29 |
| 0x38 | INT_ENABLE | [6] WOM_EN; [4] FIFO_OFLOW_EN; [3] FSYNC_INT_EN; **[0] RAW_RDY_EN** (data-ready to the INT pin); others reserved | 0x00 | RM p.29–30 |
| 0x3A | INT_STATUS (read to clear) | [6] WOM_INT; [4] FIFO_OFLOW_INT (oldest data dropped); [3] FSYNC_INT; [1] DMP_INT; [0] RAW_DATA_RDY_INT | 0x00 | RM p.30 |
| 0x3B–0x48 | 14-byte data block | ACCEL_X/Y/Z (H, L), TEMP (H, L), GYRO_X/Y/Z (H, L), big-endian two's complement. Same layout as the MPU-6050, so the M2 burst read is unchanged | "0x00 (if sensor disabled)" | RM p.7, RM p.31–35 |
| 0x68 | SIGNAL_PATH_RESET | [2] GYRO_RST; [1] ACCEL_RST; [0] TEMP_RST (sensor registers not cleared) | 0x00 | RM p.39 |
| 0x6A | USER_CTRL | [7] DMP_EN; [6] FIFO_EN; [5] I2C_MST_EN; [4] I2C_IF_DIS (SPI only); [3] DMP_RST; [2] FIFO_RST; [1] I2C_MST_RST; [0] SIG_COND_RST (also clears the sensor registers) | 0x00 | RM p.40 |
| 0x6B | PWR_MGMT_1 | [7] DEVICE_RESET (self-clearing); [6] SLEEP; [5] CYCLE; [4] GYRO_STANDBY; [3] TEMP_DIS; [2:0] CLKSEL (0 and 6 = internal 20 MHz oscillator; 1–5 = auto-select, PLL if ready; 7 = clock stopped) | **0x01** (awake, CLKSEL = 1) | RM p.41, RM p.9 |
| 0x6C | PWR_MGMT_2 | [5:3] DISABLE_XA/YA/ZA; [2:0] DISABLE_XG/YG/ZG. [7:6] LP_WAKE_CTRL exists only in MPU-6050 compatible mode | 0x00 | RM p.42, RM p.8 |
| 0x72–0x73 | FIFO_COUNT_H / L | 13-bit FIFO_CNT[12:8] / [7:0], in bytes. **Reading FIFO_COUNT_H latches both bytes**, so read H then L (a 2-byte burst from 0x72 does this) | 0x00 | RM p.43 |
| 0x74 | FIFO_R_W | FIFO data port. Reading an empty FIFO returns the last byte again, so check FIFO_COUNT first | 0x00 | RM p.43–44 |
| 0x75 | WHO_AM_I | [7:0] = 0x70 | 0x70 | RM p.44 |

Sensitivities: gyro 131 / 65.5 / 32.8 / 16.4 LSB/(°/s) for FS_SEL 0–3 (PS p.7). Accel 16 384 / 8 192 / 4 096 / 2 048 LSB/g for AFS_SEL 0–3 (PS p.8). Temperature 333.87 LSB/°C, 0 LSB at 21 °C (PS p.10, RM p.33).

**SPI-only reset note.** When using SPI, reset with DEVICE_RESET = 1, wait 100 ms, set GYRO_RST = ACCEL_RST = TEMP_RST = 1, then wait 100 ms (RM p.42). The register map gives no equivalent sequence for I²C.

## 3. Low-pass filter tables

### 3.1 Gyroscope (and temperature): CONFIG.DLPF_CFG with GYRO_CONFIG.FCHOICE_B (RM p.14)

FCHOICE in the table is the inverse of FCHOICE_B (RM p.13).

| FCHOICE_B[1:0] | DLPF_CFG | Gyro bandwidth (Hz) | Gyro delay (ms) | Rate Fs (kHz) | Temp bandwidth (Hz) | Temp delay (ms) |
|---|---|---|---|---|---|---|
| x1 | x | 8800 | 0.064 | 32 | 4000 | 0.04 |
| 10 | x | 3600 | 0.11 | 32 | 4000 | 0.04 |
| 00 | 0 | 250 | 0.97 | 8 | 4000 | 0.04 |
| 00 | 1 | **184** | **2.9** | 1 | 188 | 1.9 |
| 00 | 2 | 92 | 3.9 | 1 | 98 | 2.8 |
| 00 | 3 | 41 | 5.9 | 1 | 42 | 4.8 |
| 00 | 4 | 20 | 9.9 | 1 | 20 | 8.3 |
| 00 | 5 | 10 | 17.85 | 1 | 10 | 13.4 |
| 00 | 6 | 5 | 33.48 | 1 | 5 | 18.6 |
| 00 | 7 | 3600 | 0.17 | 8 | 4000 | 0.04 |

The product specification gives the same rates: 32 kHz for FCHOICE = 0, 1, 2; 8 kHz for DLPF_CFG 0 or 7; and 1 kHz for DLPF_CFG 1–6, all with SMPLRT_DIV = 0 (PS p.11). It quotes a programmable range of 5–250 Hz (PS p.7). Filter order and stop-band roll-off are not stated.

### 3.2 Accelerometer, normal mode: ACCEL_CONFIG2 (RM p.15)

| ACCEL_FCHOICE_B | A_DLPF_CFG | Bandwidth (Hz) | Delay (ms) | Noise density (µg/√Hz) | Output rate (kHz) |
|---|---|---|---|---|---|
| 1 | x | 1130 | 0.75 | 220 | 4 |
| 0 | 0 | 460 | 1.94 | 220 | 1 |
| 0 | 1 | 184 | 5.80 | 220 | 1 |
| 0 | 2 | 92 | 7.80 | 220 | 1 |
| 0 | 3 | 41 | 11.80 | 220 | 1 |
| 0 | 4 | 20 | 19.80 | 220 | 1 |
| 0 | 5 | 10 | 35.70 | 220 | 1 |
| 0 | 6 | 5 | 66.96 | 220 | 1 |
| 0 | 7 | 460 | 1.94 | 220 | 1 |

The output rate can be divided further by 1 / (1 + SMPLRT_DIV) (RM p.15). The two documents disagree on accelerometer noise: the register map gives 220 µg/√Hz (RM p.15) and the product specification 300 µg/√Hz (PS p.8). The accelerometer's normal-mode output range is 4 Hz to 4 kHz (PS p.8). The low-power (duty-cycled) mode table is on RM p.16; we don't use that mode.

## 4. Sample rate versus DLPF setting

- With FCHOICE_B = 00 and DLPF_CFG 1–6, the internal rate is 1 kHz, and SMPLRT_DIV divides it: SAMPLE_RATE = 1 kHz / (1 + SMPLRT_DIV) (RM p.12, PS p.11).
- With DLPF_CFG 0 or 7, the gyro rate is 8 kHz and **SMPLRT_DIV has no effect** (RM p.12, RM p.14, PS p.11).
- With FCHOICE_B ≠ 00 (DLPF bypassed), the gyro rate is 32 kHz, with 3600 Hz or 8800 Hz bandwidth. SMPLRT_DIV has no effect (RM p.12, RM p.14, PS p.11).
- SMPLRT_DIV also sets the FIFO sample rate (RM p.12). The rate at which the FIFO is written in the 8 kHz and 32 kHz modes is not stated beyond "at the sample rate" (RM p.18).
- How the accelerometer output rate (1 kHz, or 4 kHz when bypassed) lines up with an 8 kHz or 32 kHz gyro rate, and which of the two triggers data-ready in those modes, is not stated.
- **Reset state.** CONFIG = 0x00 and GYRO_CONFIG = 0x00 give DLPF_CFG = 0: 250 Hz bandwidth at 8 kHz, not 1 kHz. A driver must write CONFIG explicitly (RM p.13–14).
- Sample-clock accuracy: with the PLL (CLKSEL 1–5), ±1 % initial at 25 °C and ±1 % over temperature. With the internal oscillator (CLKSEL 0 or 6), ±2 % initial and ±10 % over temperature (PS p.11).

## 5. Data-ready interrupt

- Enable it with INT_ENABLE.RAW_RDY_EN = 1 (bit 0, RM p.30). Its timing depends on I2C_MST_CTRL.WAIT_FOR_ES (bit 6, RM p.19), which stays 0 when there are no auxiliary sensors.
- The pin is active high and push-pull at reset. ACTL and OPEN change that (RM p.29). The output high level is at least 0.9 × VDDIO (PS p.10).
- With LATCH_INT_EN = 0, each event is a **50 µs pulse** (typical, PS p.10, RM p.29). With LATCH_INT_EN = 1, the pin stays asserted until the status is cleared (RM p.29).
- Clearing: INT_STATUS is read-to-clear (RM p.30). With INT_ANYRD_2CLEAR = 1, any read clears it, including the 14-byte burst from 0x3B. With 0, only an INT_STATUS read clears it (RM p.29).
- Whether a new pulse is generated while RAW_DATA_RDY_INT is still set (pulse mode, not cleared) is not stated. Setting INT_ANYRD_2CLEAR = 1 avoids depending on it.

## 6. FIFO

- Size: **512 bytes** (PS p.5, PS p.6, PS p.24). The register map doesn't state the size. FIFO_CNT is 13 bits wide (RM p.43), and ACCEL_CONFIG2[7:4] is reserved (RM p.15), so there's no documented size option.
- Contents and order: whatever FIFO_EN selects, written in register order (lowest address first) at the sample rate (RM p.18, RM p.44). Gyro only is **6 bytes per sample** (GYRO_XOUT..GYRO_ZOUT).
- Overflow: with CONFIG.FIFO_MODE = 0 (reset), the oldest data is overwritten and INT_STATUS.FIFO_OFLOW_INT is set. With FIFO_MODE = 1, new samples are discarded once it's full (RM p.13, RM p.30, RM p.44). Whether overwriting keeps frames aligned is not stated, so treat an overflow as corrupt data and reset the FIFO (USER_CTRL.FIFO_RST, RM p.40).
- Enabling: set USER_CTRL.FIFO_EN = 1 for serial access (RM p.40) and the sources in FIFO_EN (RM p.18). An overflow interrupt is available through INT_ENABLE.FIFO_OFLOW_EN (RM p.30).

## 7. Bus speed and start-up timing

| Item | Value | Ref |
|---|---|---|
| Maximum I²C clock | 400 kHz (Fast-mode), all registers | PS p.5, PS p.12, PS p.13, PS p.28 |
| SPI | 1 MHz for all registers; 20 MHz for reading sensor and interrupt registers only | PS p.5, PS p.12 |
| Register read/write after power-up | 11 ms typical, **100 ms maximum** | PS p.10 |
| Gyro start-up from sleep | 35 ms typical | PS p.7 |
| Accel start-up from sleep | 20 ms typical | PS p.8 |
| Accel start-up from cold start (1 ms VDD ramp) | 30 ms typical | PS p.8 |
| Gyro start-up from cold power-up | Not stated | — |
| Power-up state | Active (SLEEP = 0), CLKSEL = 1 | RM p.10, RM p.41 |
| VDD and VDDIO operating range | 1.71 – 3.45 V | PS p.9 |

## 8. DLPF recommendation for a 1 kHz, data-ready-driven loop

**Settings with exactly 1 kHz output** (FCHOICE_B = 00, SMPLRT_DIV = 0; RM p.12, RM p.14):

| DLPF_CFG | Bandwidth (Hz) | Delay (ms) |
|---|---|---|
| 1 | 184 | 2.9 |
| 2 | 92 | 3.9 |
| 3 | 41 | 5.9 |
| 4 | 20 | 9.9 |
| 5 | 10 | 17.85 |
| 6 | 5 | 33.48 |

**None of these is under 2 ms.** The lowest is 2.9 ms at 184 Hz, before adding the I²C read (about 0.4 ms) and processing.

**Only one gyro setting is under 2 ms: DLPF_CFG = 0** (250 Hz, 0.97 ms). It runs at 8 kHz, ignores SMPLRT_DIV, and so raises data-ready at 8 kHz (RM p.12, RM p.14). A 1 kHz loop would act on every 8th data-ready pulse. Total gyro delay is then about 0.97 ms of filter + up to 0.125 ms sample age + about 0.4 ms of I²C read, roughly **1.5 ms**. The costs:
- an 8 kHz interrupt on Core 1, with an ISR that only counts pulses;
- weaker anti-aliasing, because the 250 Hz bandwidth sits closer to the 500 Hz Nyquist limit (roll-off not stated);
- 7 of every 8 samples are discarded. Averaging them in the FIFO isn't possible over I²C (see §9).

DLPF_CFG = 7 (3600 Hz, 0.17 ms, 8 kHz) has no useful anti-alias filter at a 1 kHz loop rate.

**Recommendation.** If the 2 ms ceiling is firm, use DLPF_CFG = 0 with 8:1 decimation in the ISR (GYRO_CONFIG = 0x10: ±1000 °/s, FCHOICE_B = 00). This changes the "each iteration consumes exactly one sample" design (blueprint §4.4 item 2). Re-check the anti-aliasing with the Phase 3 spectra. If the 2 ms ceiling can stretch to about 3.5 ms total, DLPF_CFG = 1 (184 Hz, 2.9 ms) keeps the one-sample-per-pulse design and stronger filtering.

**Accelerometer DLPF.** ACCEL_CONFIG2 = 0x03: A_DLPF_CFG = 3, 41 Hz, 11.8 ms, 1 kHz (RM p.15). The accelerometer feeds the Mahony gravity reference, which is slow by design, and the altitude Kalman input, where vertical dynamics are a few hertz and the ToF itself lags about 20 ms. Neither needs more than about 40 Hz of bandwidth, and both benefit from rejecting motor vibration, which otherwise aliases from the 460 Hz reset bandwidth into a 1 kHz output. The next setting down (20 Hz) adds 19.8 ms, which starts to matter for the altitude filter.

**Blueprint figures.** "DLPF_CFG = 1, about 188 Hz, about 1.9 ms" **does not hold for the MPU-6500 gyro.** On this chip DLPF_CFG = 1 is 184 Hz and 2.9 ms. 188 Hz / 1.9 ms are the **temperature sensor** figures in the same table row (RM p.14), and they match the MPU-6050 gyro. Likewise the blueprint's "94/98 Hz, about 2.8 ms" is the temperature column; the gyro gets 92 Hz and 3.9 ms.

## 9. FIFO feasibility for Phase 3 step 5 (wideband gyro capture)

- **"DLPF disabled at 8 kHz" is not a setting on this chip.** Bypassing the DLPF (FCHOICE_B ≠ 00) gives 32 kHz. The 8 kHz settings are DLPF_CFG = 0 (250 Hz, not wideband) and **DLPF_CFG = 7 (3600 Hz, 0.17 ms)**, which is the wideband 8 kHz choice and keeps the bandwidth below the 4 kHz Nyquist limit (RM p.14).
- FIFO size: **512 bytes**, not the 1024 bytes in the blueprint (PS p.5, PS p.6, PS p.24).
- Gyro-only sample: 6 bytes (RM p.18).
- Fill time at 8 kHz, 3 axes: 48 000 B/s, so 512 B fills in **10.7 ms** (85 samples).
- **The I²C bus can't keep up, whatever the drain period.** At 9 SCL clocks per byte, 48 000 B/s needs at least 432 000 clocks/s for the data alone. That's more than the 400 kHz maximum (PS p.12), before any address or FIFO_COUNT overhead. A task draining every 10–20 ms would see the FIFO fill in 10.7 ms and overflow (oldest data overwritten, RM p.44), and the bus couldn't move the data anyway.
- Bursts of 85 samples with gaps between them are not contiguous. A 1024-point Welch window at 8 kHz needs 128 ms of contiguous data.
- **What does work: one gyro axis at a time.** That's 2 B × 8000 = 16 000 B/s, about 144 000 clocks/s, roughly 36 % of the bus. The FIFO fills in 32 ms (256 samples). A 10 ms drain moves about 160 B, roughly 3.6 ms of bus time plus overhead.
  - Set FIFO_MODE = 1 (RM p.13), check FIFO_OFLOW_INT (RM p.30) and reset on overflow (RM p.40).
  - Run the capture three times, once per axis, with no other traffic on bus A.
  - Two axes (32 000 B/s, about 72 % of the bus) is possible on paper but tight.
- Alternative: SPI reads sensor registers at up to 20 MHz (PS p.12), but that needs SPI wiring to the IMU and a change to the bus plan.
- **The Phase 3 step 5 plan needs changing.** See diff D-IMU-13.

## 10. Other datasheet points that affect our assumptions

1. **The sleep bit after power-up.** The MPU-6500 powers up awake (PWR_MGMT_1 = 0x01, SLEEP = 0, CLKSEL = 1; RM p.9, RM p.10, RM p.41). The MPU-6050 habit of writing 0x00 to "wake" it would set CLKSEL = 0, the internal 20 MHz oscillator, worsening the sample clock from ±1 % to ±10 % over temperature (PS p.11). Write 0x01, or 0x80 to reset first.
2. **Reset values that matter.**
   - CONFIG and GYRO_CONFIG reset to 0x00, which gives **8 kHz** data-ready with DLPF_CFG = 0, not 1 kHz (RM p.12, RM p.14). Configure them before enabling RAW_RDY_EN, or the ISR runs at 8 kHz.
   - ACCEL_CONFIG2 resets to 460 Hz (RM p.15).
   - INT_PIN_CFG resets to push-pull, active high, 50 µs pulse, cleared only by reading INT_STATUS (RM p.29).
   - FIFO and DMP are off (RM p.40).
3. **WHO_AM_I is 0x70, not 0x68** (RM p.44), so the M2 acceptance criterion 3 and the M3 criterion need updating. The I²C address is still 0x68 with AD0 low (PS p.10).
4. **INT rate tolerance.** The PLL clock is ±1 % (PS p.11), so data-ready runs at 990–1010 Hz. That's looser than M3's "INT rate 1000 ± 1 Hz". The blueprint's Δt is accurate to ±1 % unless it's measured.
5. **The DLPF is digital** ("Digitally-programmable low-pass filter", PS p.6), applied at the 8 or 32 kHz internal rate before the output rate is set (RM p.14). The blueprint calls it "analogue-domain". Its job (protecting the 1 kHz output from aliasing) is unchanged; the wording is wrong.
6. **Separate accelerometer filter.** The MPU-6050 shared DLPF_CFG between gyro and accel. The MPU-6500 has a separate accelerometer filter in ACCEL_CONFIG2 (RM p.15), which the blueprint never sets.
7. **Start-up waits.** Allow up to 100 ms after power-up before the first register access (PS p.10), and 35 ms after waking from sleep before trusting the gyro (PS p.7). M2 probes the IMU after ESP32 boot and does not delay. Boot normally takes longer than 100 ms, but that isn't guaranteed.
8. **Clock source naming.** There is no "gyro X PLL" selection. CLKSEL 1–5 all mean "auto-select, PLL if ready" (RM p.41).
9. **Vibration.** The gyro's mechanical (drive) frequency is 25–29 kHz (PS p.7) and rate noise is 0.01 °/s/√Hz with 0.1 °/s RMS at 92 Hz bandwidth (PS p.7). Vibration rejection and end-of-life status are not stated in either document.
10. **Unchanged from the MPU-6050 assumptions:**
    - the data block at 0x3B (14 bytes, same order; RM p.7);
    - full-scale and sensitivity values (PS p.7–8);
    - 400 kHz maximum I²C clock (PS p.12);
    - the 0x68 address (PS p.10);
    - the burst-read protocol with a repeated START (PS p.31).

    The M2 soak read (0x3B, 14 bytes) is valid as written.

## 11. Proposed blueprint diffs

These assume the bench run confirms WHO_AM_I = 0x70. **The blueprint is not edited until approved.**

| ID | Section | Old value | New value | Reason |
|---|---|---|---|---|
| D-IMU-01 | §2.1 Inventory | IMU: MPU-6050 on a breakout | IMU: MPU-6500 (WHO_AM_I 0x70) on a breakout | RM p.44 |
| D-IMU-02 | §3 bus table, §3 pin table, §3 task diagram, M3 roadmap row, Phase 2 placement, R1 (and wherever else the IMU is named) | "MPU-6050" | "MPU-6500" | Same as D-IMU-01. Address 0x68 and 400 kHz still hold (PS p.10, PS p.12) |
| D-IMU-03 | §4.4 item 1, PWR_MGMT_1 | "wake, clock source = gyro X PLL" | "Write 0x01 (SLEEP = 0, CLKSEL = 1, auto-select PLL). The part already powers up in this state. Never write 0x00 (internal oscillator, ±10 % over temperature). Wait ≥ 100 ms after power-up before the first access" | RM p.9, RM p.10, RM p.41; PS p.10, PS p.11 |
| D-IMU-04 | §4.4 item 1, SMPLRT_DIV | "= 0 gives a 1 kHz output rate when the DLPF is enabled" | "= 0 gives 1 kHz only when FCHOICE_B = 00 and DLPF_CFG = 1–6. Ignored for DLPF_CFG 0/7 (8 kHz) and when bypassed (32 kHz)" | RM p.12, PS p.11 |
| D-IMU-05 | §4.4 item 1, CONFIG | "DLPF_CFG = 1 (about 188 Hz gyro bandwidth, about 1.9 ms delay) … 94/98 Hz cost about 2.8 ms" | "DLPF_CFG = 1 is 184 Hz, 2.9 ms; DLPF_CFG = 2 is 92 Hz, 3.9 ms. (188 Hz/1.9 ms and 98 Hz/2.8 ms are the temperature-sensor column.) Value chosen per D-IMU-06" | RM p.14 |
| D-IMU-06 | §4.4 item 2 and the CLAUDE.md key decision "DLPF 188 Hz" | 1 kHz data-ready, one sample per iteration, about 188 Hz/1.9 ms | Option A (recommended if the 2 ms ceiling is firm): DLPF_CFG = 0 (250 Hz, 0.97 ms, 8 kHz data-ready); ISR wakes `flight_task` on every 8th pulse; total gyro delay about 1.5 ms. Option B: DLPF_CFG = 1 (184 Hz, 2.9 ms), one sample per pulse, total about 3.5 ms | No 1 kHz setting is under 2 ms (RM p.14). SMPLRT_DIV is ignored at DLPF_CFG = 0 (RM p.12) |
| D-IMU-07 | §4.4 item 1, GYRO_CONFIG | "±1000 °/s, 32.8 LSB/(°/s)" | Same, plus "write 0x10 so FCHOICE_B[1:0] = 00; any other FCHOICE_B bypasses the DLPF and SMPLRT_DIV" | RM p.12, RM p.14; PS p.7 |
| D-IMU-08 | §4.4 item 1 (new line) | Accelerometer DLPF not configured | "ACCEL_CONFIG2 (0x1D) = 0x03: 41 Hz, 11.8 ms, 1 kHz. Reset value is 460 Hz" | Separate accel filter on the MPU-6500 (RM p.15). Rationale in §8 |
| D-IMU-09 | §4.4 item 1, INT_PIN_CFG / INT_ENABLE | "INT_ENABLE (0x38, bit 0 DATA_RDY_EN): data-ready pulse" | "INT_ENABLE = 0x01 (bit 0 RAW_RDY_EN). INT_PIN_CFG = 0x10: push-pull, active high, 50 µs pulse, INT_ANYRD_2CLEAR = 1 so the 14-byte burst clears the status. Configure CONFIG first (reset value gives 8 kHz)" | RM p.29, RM p.30, RM p.14; PS p.10 |
| D-IMU-10 | §4.4 item 1, WHO_AM_I | "WHO_AM_I (0x75) reads 0x68" | "reads 0x70" | RM p.44 |
| D-IMU-11 | §4.4 item 3 | "Hardware DLPF: the sole analogue-domain defence" | "On-chip digital DLPF, applied at the 8/32 kHz internal rate before the output rate is set: the only defence against noise above the loop's Nyquist limit" | PS p.6, RM p.14 |
| D-IMU-12 | §4.4 item 2 ("clean Δt") and M3 roadmap row ("INT rate 1000 ± 1 Hz") | Exact 1 kHz; acceptance 1000 ± 1 Hz | "Sensor clock ±1 % (PLL): acceptance 1000 ± 10 Hz, and Δt is measured, not assumed" (with Option A: an 8000 ± 80 Hz pulse rate, 1 kHz after decimation) | PS p.11 |
| D-IMU-13 | Phase 3 step 5 | "gyro with the DLPF disabled at 8 kHz via the MPU-6050 FIFO (1024 B, drained in bursts)" | "Gyro at 8 kHz with FCHOICE_B = 00, DLPF_CFG = 7 (3600 Hz). FIFO is 512 B. Capture **one axis at a time** (16 kB/s), FIFO_MODE = 1, drained every 10 ms, with overflow detection and nothing else on bus A. Three runs cover X, Y and Z. Contiguous 3-axis 8 kHz capture over I²C is impossible (needs > 432 kHz SCL); it would need SPI" | PS p.5, PS p.12, PS p.24; RM p.13, RM p.14, RM p.18, RM p.44 |
| D-IMU-14 | Phase 3 step 7 and M3 roadmap row | "188 Hz versus lower"; "188 Hz DLPF" | "184 Hz (DLPF_CFG = 1) or 250 Hz (DLPF_CFG = 0) per D-IMU-06, versus lower" | RM p.14 |
| D-IMU-15 | §6.2 R1 | "the MPU-6050 is end-of-life and vibration-sensitive" | "MPU-6500: gyro noise 0.01 °/s/√Hz, mechanical drive 25–29 kHz; vibration sensitivity and lifecycle status not stated in the datasheets." Add "accel DLPF (D-IMU-08)" to the mitigation | PS p.7; RM p.15 |
| D-IMU-16 | M3 roadmap row | "`WHO_AM_I` = 0x68" | "`WHO_AM_I` = 0x70" | RM p.44 |

Related points outside the requested blueprint sections (not part of the diff list above):

- §2.2 records the pull-up measurement for "MPU-6050" as open. If the breakout is a different board, measure it again.
- The M2 package needs matching edits: §1 goal 1, §2 wiring labels, and §6 criterion 3 ("WHO_AM_I = 0x68" becomes "0x68 or 0x70").
