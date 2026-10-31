# Franken-E58 project guide

Closed-loop micro-quadcopter: ESP32-S3-N16R8 on a Mavic E58 frame. Firmware is ground-up ESP-IDF 5.x (PlatformIO, official `espressif32` platform, `framework = espidf`, no Arduino). The binding spec is `docs/Franken-E58_Full_System_Architecture_Blueprint.md`. Read the sections relevant to the current module before starting. Do not load the whole file unless it is needed.

## Status
- Phase 1 (breadboard, props off, no motors). M4 passed; next M5. M4 record: `docs/M4_package.md`.
- Work on ONE module at a time. Never add features from later modules.

## Commands
- Build: `pio run`
- Clean: `pio run -t clean`
- Ports: `pio device list`
- Config check: `tools/verify_sdkconfig.sh` (run after a build)
- Never run `pio run -t upload` or `pio device monitor`. The user flashes and monitors manually.

## Key decisions (do not change without a blueprint diff)
- ToF is a VL53L1X. MPU-6500 on I2C0, VL53L1X on I2C1.
- 1 kHz loop, INT-driven IMU sampling, DLPF_CFG as a compile-time constant (default 1: 184 Hz).
- WebSocket for the browser joystick, raw UDP for Python tools and telemetry.
- Mahony attitude filter plus a 3-state altitude Kalman filter.
- Flight modes: Rate, then Angle, then Altitude. Angle is proven on the tether before free flight.
- No flash writes while armed. No PSRAM in flight code or data.
- Platform pinned to `espressif32 @ 6.13.0` (ESP-IDF 5.5.3).
- Use split component names (`esp_driver_gpio`, `esp_driver_i2c`, `esp_driver_mcpwm`, `esp_driver_gptimer`, `esp_adc`), never the legacy `driver` catch-all.
- Flash mode is DIO.
- I2C bus recovery by tear-down and rebuild is a bench-test exception and must never run while armed. Flight code relies on the IMU-stall cut trigger. Investigate i2c_master_bus_reset() in M3.

## Code rules
- C++17 for firmware, Python 3 for tools. British English in comments and docs.
- All drivers use ESP-IDF 5.x APIs directly, behind HAL components (`hal_i2c`, `hal_mcpwm`, ...). Drivers never call IDF peripheral APIs directly.
- Pin numbers and limits live ONLY in `components/board_config`.
- Static allocation only after initialisation. No heap allocation after initialisation, and no printf or ESP_LOG, on Core 1.
- ISRs and control paths are IRAM_ATTR and do the minimum (notify, timestamp).
- Lock-free hand-offs between cores (SPSC ring, atomic slot, seqlock). Never share a user-level mutex, semaphore or queue with Core 0. The FreeRTOS kernel spinlock taken by task notifications is unavoidable; its effect on wake-up latency is measured in M3 (Wi-Fi off) and M7 (Wi-Fi on).
- Every module ships a bench test and a timing measurement hook.

## Safety
- Props are off for all bring-up. Never write code that enables motor output without the arming state machine and the 150 ms link-loss cutoff.
- Emergency cut: duty to zero and MCPWM generators forced low, latched, no automatic re-arm.

## Working style
- For anything touching more than one file, show a plan first and wait for approval.
- Keep diffs minimal. List every deviation from the module spec in the final report.
- Mark any register address, config name or constant recalled from memory with `// verify against datasheet` or `// verify against IDF docs`.
- If a request conflicts with the blueprint or with datasheet reality, stop and report it as a diff (ID, old value, new value, reason). Never silently deviate.
- After changing `sdkconfig.defaults`, delete `sdkconfig.esp32s3`, rebuild, and run `tools/verify_sdkconfig.sh`.
