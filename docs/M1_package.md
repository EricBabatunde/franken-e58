# Franken-E58 Phase 1, Module 1: Project Skeleton, Console Logging, Safe Pins, LED Probe

**Scope:** PlatformIO project skeleton, `sdkconfig.defaults`, dual-console logging (UART and native USB), GPIO 4 to 7 driven low at boot, and an LED probe that finds the onboard LED. No sensors, no motors, no Wi-Fi.

**Not compiled.** This code was written without access to your pinned toolchain, so treat it as a reviewed draft. Claude Code's first job is to build it and fix compile errors with the smallest possible change, reporting each one. Anything marked *verify* is recalled from memory and must be checked against the pinned ESP-IDF version.

---

## 1. Goals

1. The project builds and flashes with the official `espressif32` platform, pure ESP-IDF (`framework = espidf`).
2. Timestamped log lines appear over the UART socket **and** the native USB socket.
3. Every `sdkconfig.defaults` entry is confirmed present in the generated `sdkconfig`. Unknown names are silently dropped by Kconfig, so this check is mandatory.
4. GPIO 4 to 7 are driven low as outputs at boot.
5. The onboard LED GPIO is identified (38 or 48) and its type noted (plain LED or WS2812 RGB).
6. The native USB socket is identified (`lsusb` shows `303a:1001`).

## 2. File tree

```
franken-e58/
├── CLAUDE.md
├── .claude/
│   └── settings.json
├── .gitignore
├── platformio.ini
├── sdkconfig.defaults
├── partitions.csv
├── CMakeLists.txt
├── src/
│   ├── CMakeLists.txt
│   ├── main.cpp
│   ├── led_probe.h
│   └── led_probe.cpp          (bring-up tool, deleted before Module 7)
├── components/
│   ├── board_config/
│   │   ├── CMakeLists.txt
│   │   └── include/board_config.h
│   └── hal_gpio/
│       ├── CMakeLists.txt
│       ├── include/hal_gpio.h
│       └── hal_gpio.cpp
├── tools/
│   └── verify_sdkconfig.sh
└── docs/
    ├── Franken-E58_Full_System_Architecture_Blueprint.md
    └── M1_package.md          (this file)
```

## 3. Files

### `platformio.ini`

```ini
[platformio]
default_envs = esp32s3

[env:esp32s3]
; Official platform, pure ESP-IDF. After the first successful build run `pio pkg list`,
; then pin the platform: platform = espressif32 @ <version shown>
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf

; N16R8: 16 MB flash, octal PSRAM. The memory_type key comes from the board manifest (verify it
; has an effect in pure ESP-IDF); sdkconfig.defaults sets the same modes explicitly.
board_upload.flash_size = 16MB
board_build.arduino.memory_type = qio_opi
board_build.partitions = partitions.csv
board_build.filesystem = littlefs

upload_speed = 921600
monitor_speed = 115200
monitor_filters = default, esp32_exception_decoder
```

### `sdkconfig.defaults`

```
# Target and memory
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y

# CPU and scheduler
CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240=y
CONFIG_FREERTOS_HZ=1000
CONFIG_COMPILER_OPTIMIZATION_PERF=y

# Core affinity: everything asynchronous lives on Core 0
CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y
CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y
CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0=y

# Console: UART0 primary, native USB Serial/JTAG secondary (both print logs)
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y

# Safety layers
CONFIG_ESP_BROWNOUT_DET=y
CONFIG_ESP_TASK_WDT_EN=y
```

### `partitions.csv`

```
# Name,    Type, SubType,  Offset,   Size
nvs,       data, nvs,      0x9000,   0x6000
phy_init,  data, phy,      0xf000,   0x1000
factory,   app,  factory,  0x10000,  0x300000
storage,   data, spiffs,   0x310000, 0x100000
```

The `storage` partition uses subtype `spiffs` because the LittleFS component mounts by partition label (*verify* in Module 5).

### `CMakeLists.txt` (project root)

```cmake
cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(franken-e58)
```

### `src/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "main.cpp" "led_probe.cpp"
    INCLUDE_DIRS "."
    REQUIRES board_config hal_gpio
    PRIV_REQUIRES driver esp_timer esp_system esp_hw_support heap)
```

### `components/board_config/CMakeLists.txt`

```cmake
idf_component_register(INCLUDE_DIRS "include")
```

### `components/board_config/include/board_config.h`

```cpp
#pragma once
// Single source of truth for pin numbers and project-wide limits.
// No other file may contain a literal GPIO number.

namespace board {

// Motors (MCPWM in a later module): M1 FL, M2 FR, M3 RL, M4 RR
constexpr int kMotorGpio[4] = {4, 5, 6, 7};

// Bus A (I2C0): MPU-6050
constexpr int kImuSda = 1;
constexpr int kImuScl = 2;
constexpr int kImuInt = 9;

// Bus B (I2C1): VL53L1X
constexpr int kTofSda   = 11;
constexpr int kTofScl   = 12;
constexpr int kTofXshut = 8;
constexpr int kTofInt   = 13;  // optional

// Battery sense (ADC1_CH9)
constexpr int kVbatAdc = 10;

// Possible onboard LED pins, reserved until identified by the M1 probe
constexpr int kLedCandidates[2] = {38, 48};

}  // namespace board

// Set to 0 once the LED GPIO is identified.
#define FE_LED_PROBE 1
```

### `components/hal_gpio/CMakeLists.txt`

```cmake
idf_component_register(
    SRCS "hal_gpio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board_config
    PRIV_REQUIRES driver)
```

### `components/hal_gpio/include/hal_gpio.h`

```cpp
#pragma once
#include "esp_err.h"

namespace hal_gpio {

// Configure the four motor gate pins as outputs and drive them low.
// Must run before any other code touches these pins.
esp_err_t motor_pins_init_low();

}  // namespace hal_gpio
```

### `components/hal_gpio/hal_gpio.cpp`

```cpp
#include "hal_gpio.h"

#include <cstdint>

#include "board_config.h"
#include "driver/gpio.h"

namespace hal_gpio {

esp_err_t motor_pins_init_low() {
    uint64_t mask = 0;
    for (int pin : board::kMotorGpio) {
        mask |= (1ULL << pin);
    }

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = mask;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;     // never pull a gate high
    cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;  // belt and braces beside the 10 kΩ
    cfg.intr_type = GPIO_INTR_DISABLE;

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    for (int pin : board::kMotorGpio) {
        err = gpio_set_level(static_cast<gpio_num_t>(pin), 0);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

}  // namespace hal_gpio
```

### `src/led_probe.h`

```cpp
#pragma once
// Bring-up tool. Not part of the flight stack. Delete before Module 7.
void led_probe_run();
```

### `src/led_probe.cpp`

```cpp
#include "led_probe.h"

#include "board_config.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "led_probe";

static void delay_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

// Phase A: plain GPIO toggle (lights a single-colour LED wired to the pin).
static void plain_blink(gpio_num_t pin) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
    for (int i = 0; i < 6; ++i) {
        gpio_set_level(pin, i & 1);
        delay_ms(250);
    }
    gpio_set_level(pin, 0);
    gpio_reset_pin(pin);
}

// Phase B: one WS2812 frame via the RMT peripheral (addressable RGB LED).
// Timings at 10 MHz: bit0 = 0.3 us high / 0.9 us low, bit1 = 0.9 us / 0.3 us (verify).
static bool ws2812_set(gpio_num_t pin, uint8_t r, uint8_t g, uint8_t b) {
    rmt_channel_handle_t ch = nullptr;
    rmt_encoder_handle_t enc = nullptr;

    rmt_tx_channel_config_t cc = {};
    cc.gpio_num = pin;
    cc.clk_src = RMT_CLK_SRC_DEFAULT;
    cc.resolution_hz = 10 * 1000 * 1000;
    cc.mem_block_symbols = 64;
    cc.trans_queue_depth = 1;
    if (rmt_new_tx_channel(&cc, &ch) != ESP_OK) {
        return false;
    }

    rmt_bytes_encoder_config_t ec = {};
    ec.bit0.level0 = 1; ec.bit0.duration0 = 3;
    ec.bit0.level1 = 0; ec.bit0.duration1 = 9;
    ec.bit1.level0 = 1; ec.bit1.duration0 = 9;
    ec.bit1.level1 = 0; ec.bit1.duration1 = 3;
    ec.flags.msb_first = 1;
    if (rmt_new_bytes_encoder(&ec, &enc) != ESP_OK) {
        rmt_del_channel(ch);
        return false;
    }

    bool ok = (rmt_enable(ch) == ESP_OK);
    if (ok) {
        const uint8_t grb[3] = {g, r, b};
        rmt_transmit_config_t tc = {};
        ok = (rmt_transmit(ch, enc, grb, sizeof(grb), &tc) == ESP_OK) &&
             (rmt_tx_wait_all_done(ch, 100) == ESP_OK);
        rmt_disable(ch);
    }
    rmt_del_encoder(enc);
    rmt_del_channel(ch);
    return ok;
}

void led_probe_run() {
    for (int pin_num : board::kLedCandidates) {
        const gpio_num_t pin = static_cast<gpio_num_t>(pin_num);

        ESP_LOGI(TAG, "PROBE A: plain toggle on GPIO %d (3 blinks)", pin_num);
        plain_blink(pin);
        delay_ms(500);

        ESP_LOGI(TAG, "PROBE B: WS2812 on GPIO %d (red, green, blue, off)", pin_num);
        const uint8_t colours[4][3] = {{32, 0, 0}, {0, 32, 0}, {0, 0, 32}, {0, 0, 0}};
        for (const auto& c : colours) {
            if (!ws2812_set(pin, c[0], c[1], c[2])) {
                ESP_LOGW(TAG, "WS2812 transmit failed on GPIO %d", pin_num);
                break;
            }
            delay_ms(600);
        }
        gpio_reset_pin(pin);
        delay_ms(500);
    }
    ESP_LOGI(TAG, "PROBE DONE: report which phase and GPIO lit the LED (or none)");
}
```

### `src/main.cpp`

```cpp
#include <cstdint>

#include "board_config.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal_gpio.h"
#include "led_probe.h"

static const char* TAG = "main";

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_USB:      return "USB";
        default:               return "other";
    }
}

extern "C" void app_main(void) {
    // 1. Safe state first: motor gates low before anything else runs.
    const esp_err_t gpio_err = hal_gpio::motor_pins_init_low();

    // 2. Banner.
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "Franken-E58 M1 boot. IDF %s, %d core(s), rev %d",
             esp_get_idf_version(), chip.cores, chip.revision);
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_str(esp_reset_reason()));
    ESP_LOGI(TAG, "motor pins %d,%d,%d,%d low: %s",
             board::kMotorGpio[0], board::kMotorGpio[1],
             board::kMotorGpio[2], board::kMotorGpio[3],
             gpio_err == ESP_OK ? "OK" : esp_err_to_name(gpio_err));
    ESP_LOGI(TAG, "heap: internal free %u B, PSRAM free %u B",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // 3. One-off LED probe (bring-up only).
#if FE_LED_PROBE
    led_probe_run();
#endif

    // 4. Heartbeat: 1 Hz, microsecond timestamp from the 64-bit esp_timer.
    for (;;) {
        ESP_LOGI(TAG, "HB t_us=%lld internal_free=%u min_free=%u",
                 static_cast<long long>(esp_timer_get_time()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

### `tools/verify_sdkconfig.sh`

```bash
#!/usr/bin/env bash
# Confirms every CONFIG_ line in sdkconfig.defaults survived into the generated sdkconfig.
# Unknown option names are silently dropped by Kconfig, so this check is mandatory.
# Usage: tools/verify_sdkconfig.sh [generated_file]   (default: sdkconfig.esp32s3)
set -u
gen="${1:-sdkconfig.esp32s3}"
[ -f "$gen" ] || { echo "Generated file $gen not found. Run 'pio run' first."; exit 2; }
bad=0
while IFS= read -r line; do
  key="${line%%=*}"
  val="${line#*=}"
  if [ "$val" = "n" ]; then want="# $key is not set"; else want="$line"; fi
  if grep -qxF "$want" "$gen"; then
    echo "OK        $line"
  else
    echo "MISMATCH  $line"
    bad=1
  fi
done < <(grep -E '^CONFIG_' sdkconfig.defaults)
exit $bad
```

### `.gitignore`

```
.pio/
.venv/
sdkconfig.*
!sdkconfig.defaults
.claude/settings.local.json
```

### `CLAUDE.md`

```markdown
# Franken-E58 project guide

Closed-loop micro-quadcopter: ESP32-S3-N16R8 on a Mavic E58 frame. Firmware is ground-up ESP-IDF 5.x (PlatformIO, official `espressif32` platform, `framework = espidf`, no Arduino). The binding spec is `docs/Franken-E58_Full_System_Architecture_Blueprint.md`. Read the sections relevant to the current module before starting. Do not load the whole file unless it is needed.

## Status
- Phase 1 (breadboard, props off, no motors). Current module: M1, see `docs/M1_package.md`.
- Work on ONE module at a time. Never add features from later modules.

## Commands
- Build: `pio run`
- Clean: `pio run -t clean`
- Ports: `pio device list`
- Config check: `tools/verify_sdkconfig.sh` (run after a build)
- Never run `pio run -t upload` or `pio device monitor`. The user flashes and monitors manually.

## Key decisions (do not change without a blueprint diff)
- ToF is a VL53L1X. MPU-6050 on I2C0, VL53L1X on I2C1.
- 1 kHz loop, INT-driven IMU sampling, DLPF 188 Hz as a compile-time constant.
- WebSocket for the browser joystick, raw UDP for Python tools and telemetry.
- Mahony attitude filter plus a 3-state altitude Kalman filter.
- Flight modes: Rate, then Angle, then Altitude. Angle is proven on the tether before free flight.
- No flash writes while armed. No PSRAM in flight code or data.

## Code rules
- C++17 for firmware, Python 3 for tools. British English in comments and docs.
- All drivers use ESP-IDF 5.x APIs directly, behind HAL components (`hal_i2c`, `hal_mcpwm`, ...). Drivers never call IDF peripheral APIs directly.
- Pin numbers and limits live ONLY in `components/board_config`.
- Static allocation only after initialisation. No heap allocation, printf or ESP_LOG on Core 1.
- ISRs and control paths are IRAM_ATTR and do the minimum (notify, timestamp).
- Lock-free hand-offs between cores (SPSC ring, atomic slot, seqlock). Never share a mutex with Core 0.
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
```

### `.claude/settings.json`

```json
{
  "permissions": {
    "allow": [
      "Bash(pio run)",
      "Bash(pio run -t clean)",
      "Bash(pio pkg list)",
      "Bash(pio device list)",
      "Bash(tools/verify_sdkconfig.sh:*)",
      "Bash(git status)",
      "Bash(git diff:*)",
      "Bash(git log:*)"
    ],
    "deny": [
      "Bash(pio run -t upload:*)",
      "Bash(pio run --target upload:*)",
      "Bash(pio device monitor:*)",
      "Bash(rm -rf:*)",
      "Bash(git push:*)"
    ]
  }
}
```

The permission rule syntax is recalled from memory. After starting Claude Code, run `/permissions` and confirm the allow and deny lists loaded.

## 4. Claude Code prompt (paste in plan mode)

```
We're starting Phase 1, Module 1 of Franken-E58. Read CLAUDE.md, then docs/M1_package.md sections 1 to 3 (goals, tree, files). The project files already exist in this repo. Do NOT recreate them.

Task:
1. Run `pio run`. If it fails, fix the errors with the smallest possible change that keeps behaviour identical. Only edit files under src/ and components/, plus platformio.ini or sdkconfig.defaults if an error requires it. Do not add features.
2. After a clean build run tools/verify_sdkconfig.sh. For each MISMATCH, find the option name ESP-IDF actually uses (search the generated sdkconfig.esp32s3 and the Kconfig files under ~/.platformio/packages/framework-espidf) and tell me the corrected line. Do not edit sdkconfig.defaults until I approve.
3. Run `pio pkg list` and report the exact platform and framework-espidf versions.
4. Never run upload or the serial monitor.

Show me the plan first and wait for approval, then act. Final report: (a) numbered list of every change with file and reason, (b) MISMATCH list with proposed fixes, (c) version list, (d) anything in the M1 spec that looks wrong for the ESP-IDF version in use.
```

## 5. Bench procedure

**Equipment:** the ESP32-S3 board and a USB-C data cable. No sensors, no wiring, no battery.

1. Unzip the project to `~/franken-e58`, run `git init && git add -A && git commit -m "M1 skeleton"`, and open the folder in VS Code.
2. Start Claude Code in the repo (`claude`), switch to plan mode (Shift+Tab cycles the modes), paste the prompt from §4, approve the plan, and let it build. Do not upload yet.
3. Review Claude Code's report. Check the MISMATCH list and the version list.
4. **Upload over the UART socket (known good):** `pio device list` to confirm `/dev/ttyACM0`, then `pio run -t upload`.
5. Open the monitor: `pio device monitor`. Expect the banner, reset reason, `motor pins 4,5,6,7 low: OK`, the heap line, the LED probe, then one `HB` line per second.
6. **LED probe:** watch the board during the probe. Note which GPIO and which phase (A plain blink, B RGB colours) lit the LED, or whether nothing did. Also note whether the board has an RGB LED or a single-colour one.
7. **Native USB socket:** close the monitor, unplug, and plug the **other** socket only. Run `lsusb` and look for `303a:1001`. Run `ls /dev/ttyACM*` and `pio device list`. Open the monitor on that port. The same log lines should appear.
8. **Optional gate check with the multimeter:** with the firmware running (after the probe), measure DC volts on GPIO 4 to 7 against GND. Expect about 0 V.
9. Set `FE_LED_PROBE` to 0 in `board_config.h` once the LED is identified, rebuild and re-upload.
10. Record the platform version from `pio pkg list` and pin it in `platformio.ini`.

## 6. Acceptance criteria

| # | Criterion | Pass condition |
|---|---|---|
| 1 | Build | `pio run` succeeds with the pinned platform |
| 2 | Config | `verify_sdkconfig.sh` reports no MISMATCH, or each mismatch is resolved with a corrected name |
| 3 | Logging, UART socket | Timestamped lines at 1 Hz for at least 60 s with no resets |
| 4 | Logging, native socket | The same lines on the `303a:1001` port |
| 5 | Safe pins | Banner says `motor pins 4,5,6,7 low: OK`. Multimeter reads about 0 V on each |
| 6 | LED probe | LED GPIO and type recorded (or recorded as "no LED found") |
| 7 | Reset reason | `power-on` on first boot. No `BROWNOUT`, `panic` or watchdog resets |

## 7. Report back to me

- The `pio pkg list` versions.
- The MISMATCH list (or "none").
- LED result: GPIO, phase, RGB or single colour.
- Native socket result: `lsusb` line, port name, and whether logs appeared.
- Anything Claude Code changed to get it to build.
- The first 15 lines of the monitor output.

## 8. ESP-IDF APIs used and where to read about them

Use the ESP-IDF Programming Guide for the exact version `pio pkg list` reports, ESP32-S3 variant.

| API | Used for | Guide section |
|---|---|---|
| `gpio_config`, `gpio_set_level` | Motor pins low, plain LED blink | Peripherals → GPIO |
| `ESP_LOGI`, `ESP_LOGW` | Logging, with timestamps | API Reference → Logging library |
| `esp_timer_get_time` | 64-bit microsecond clock | System → High Resolution Timer |
| `esp_reset_reason` | Why the chip last reset | System → Miscellaneous System APIs |
| `heap_caps_get_free_size` | Internal versus PSRAM heap | System → Heap Memory Allocation |
| `rmt_new_tx_channel`, `rmt_new_bytes_encoder`, `rmt_transmit` | WS2812 LED probe | Peripherals → RMT |
| `app_main` and FreeRTOS `vTaskDelay` | Entry point and delay | System → FreeRTOS (IDF) |

ESP32-S3 Technical Reference Manual: read the GPIO and IO MUX chapter (why GPIO 4 to 7 are safe) and the RMT chapter (WS2812 timing).
