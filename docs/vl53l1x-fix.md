# VL53L1X on ESP32-S3: what broke and how it was fixed

A debugging record from NeuComponents (2026-10-31), written so another project, or a model working on one, can apply the same fixes. The sensor was **not damaged**. There were two separate problems:

1. **Wiring:** an I²C line was held low, so the sensor never answered.
2. **Distance mode:** the firmware used Short mode. Short mode stops at about 1.3 m, so every reading of a target farther away was rejected with `range_status` 4.

## Setup

| Part | Detail |
|------|--------|
| MCU | ESP32-S3 DevKitC-1, Arduino framework, PlatformIO (`board = esp32-s3-devkitc-1`) |
| USB | CH343 USB-UART (`1a86:55d3`), shows up as `/dev/ttyACM0` |
| Sensor | VL53L1X breakout, I²C address 0x29 |
| Library | `pololu/VL53L1X@^1.3.1` |
| Wiring | VIN→3V3, GND→GND, SDA→GPIO8, SCL→GPIO9. GPIO1 and XSHUT not connected |
| Output | One JSON line per reading at 115200 baud: `{"d":mm,"status":n,"signal":MCPS,"ambient":MCPS}` |

## The original code (the version that failed)

```cpp
Wire.begin();
Wire.setClock(400000);
sensor.setTimeout(500);
while (!sensor.init()) {
  Serial.println("VL53L1X not found. Check wiring.");
  delay(1000);
}
sensor.setDistanceMode(VL53L1X::Short);      // <-- problem 2
sensor.setMeasurementTimingBudget(20000);
sensor.startContinuous(20);
```

When `init()` failed, this code gave no clue why. It also had no way to show that readings were coming back invalid.

## Problem 1: an I²C line held low

### Symptoms
- `sensor.init()` failed and the board printed `VL53L1X not found` every second.
- The ESP32 core logged `requestFrom(): i2cRead returned Error 263`, which is a timeout.
- The first bus scan hung, then reported devices at `0x11 0x12`, which do not exist. A stuck bus produces addresses like these.

### Tests run, in order

**1. Bus scan and model ID read.** These were added to run whenever `init()` fails. The scan probes addresses 1–126. The ID check reads register `0x010F` at 0x29; a real VL53L1X returns `0xEACC` and a VL53L0X returns `0xEEAA`. Result: no real device answered.

**2. Idle line levels.** With pull-ups enabled and nothing transmitting, both SDA and SCL must read 1. Result:

| When | SDA (GPIO8) | SCL (GPIO9) |
|------|-------------|-------------|
| First test | **0** | 1 |
| After the first rewire | 1 | **0** |
| After the wiring was fixed | 1 | 1 |

At 100 kHz, every address in the scan returned error 5 (timeout) while a line was low.

**3. Bus recovery.** A device reset in the middle of a transfer can keep holding SDA low. Clocking SCL 9 times normally releases it. Here it did not, which ruled out a stuck transfer and pointed at a physical connection.

**4. Working out which pin was at fault.** The low reading moved from SDA to SCL when the wires were moved. That means one particular sensor pin was dragging down whichever ESP32 pin it was plugged into. That pin sits low: GPIO1 (interrupt output), XSHUT or GND. The planned pull-out test (unplug only the suspect wire at the sensor end and see if the ESP32 pin goes back to 1) was not needed. Re-checking each wire against the pin labels printed on the board fixed it.

**5. USB dropout.** At one point the board disappeared from USB completely. `journalctl -k` showed `usb 1-1: USB disconnect` with no reconnect. On this board the USB-UART chip shares the 3.3 V rail, so a 3V3-to-GND short in the sensor wiring can take the whole board off USB. It came back after it was replugged and stayed stable.

### Fix
Correct wiring. After that both lines read 1, the scan found `0x29`, `init()` succeeded, and the first readings were valid (status 0, about 220 mm, signal about 18 MCPS).

### Code kept from this step
- `recoverBus()` runs before `Wire.begin()`. It frees a stuck SDA and prints a clear message if a line is still low.
- `Wire.setTimeOut(20)` makes a stuck bus fail fast instead of hanging the scan.
- `diagnoseBus()` prints the scan and the model ID when `init()` fails.

## Problem 2: Short mode rejected far targets

### Symptoms
With the wiring fixed, the sensor was pointed at things farther away (the room, about 1.5–1.8 m):
- In one 6 s capture, **16 of 225** readings were valid (105–134 mm). The rest were rejected.
- In the next 5 s capture, **231 of 231** readings had `status 4`, with `d` 1494–1802 mm, signal about 4.35 MCPS and ambient about 1.49 MCPS.

The `d` value still looked reasonable, but every reading was flagged invalid, so anything that filters on `status == 0` received nothing.

### Cause
In the Pololu library, `range_status` 4 is `OutOfBoundsFail`. The sensor measured something beyond the range the current distance mode accepts.

| Mode | Max range (indoors) | Min timing budget | Notes |
|------|--------------------|-------------------|-------|
| Short | about 1.3 m | 20 ms | Best in bright ambient light |
| Medium | about 3 m | 33 ms | |
| Long | about 4 m in the dark | 33 ms | Range drops in sunlight |

### Fix
```cpp
sensor.setDistanceMode(VL53L1X::Long);
sensor.setMeasurementTimingBudget(33000);  // 33 ms is Long mode's minimum
sensor.startContinuous(33);                // inter-measurement period, ≥ budget
```

### Result
In a 5 s capture: **87 of 87** readings had status 0, with `d` 1704–1780 mm and signal about 4.85 MCPS. The rate dropped from about 50 Hz in Short mode to about 17–30 Hz. The 33 ms period gives a ceiling of about 30 Hz, and in that capture readings arrived at about 17 a second.

## How to apply this to another project

1. **Check the wiring first.** Add the line-level check. If SDA or SCL reads 0 at idle, no code change will help. Compare every wire to the label printed on the board, not to a pinout picture, because breakouts order their pins differently.
2. **Print `range_status` next to every distance.** A plausible `d` with a non-zero status is a rejected reading. If you only print `sensor.read()`, you cannot tell.
3. **Match the distance mode to the farthest target.** If the target can be beyond about 1.3 m, use Long (or Medium). If the target is always close and the light is bright, Short is better.
4. **Keep budget ≤ period.** `startContinuous(period_ms)` must be at least the timing budget in ms. Long and Medium need at least 33 ms. A longer budget, such as 50–100 ms, gives less noise and more range at a lower rate.
5. **Drop invalid readings in code**, or handle them separately: `if (sensor.ranging_data.range_status != 0) continue;`.
6. **Other status codes you may see:** 1 SigmaFail (noisy), 2 SignalFail (weak return: dark target, too far, or film still on the lens), 7 WrapTargetFail (very bright target beyond range). For 2 and 7, try Long mode or a longer budget.
7. **Only one program can read the serial port.** A second reader (a serial monitor, VS Code, a browser tab) garbles both. Close the others before testing.

## Full working firmware

`firmware/vl53l1x/src/main.cpp` in NeuComponents:

```cpp
#include <Arduino.h>
#include <Wire.h>
#include <VL53L1X.h>

VL53L1X sensor;

// Explain why init failed: list I²C devices and the model ID at 0x29 (VL53L1X = 0xEACC).
void diagnoseBus() {
  Serial.printf("I2C scan on SDA=%d SCL=%d:", SDA, SCL);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf(" 0x%02X", addr);
      found++;
    }
  }
  if (!found) Serial.print(" nothing answered (check power, GND, SDA/SCL pins, XSHUT not pulled low)");
  Serial.println();
  Wire.beginTransmission(0x29);
  Wire.write(0x01);  // model ID register 0x010F, 16-bit address
  Wire.write(0x0F);
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)0x29, (uint8_t)2) == 2) {
    uint16_t id = (Wire.read() << 8) | Wire.read();
    Serial.printf("Device at 0x29 reports model ID 0x%04X (VL53L1X = 0xEACC, VL53L0X answers 0xEEAA here)\n", id);
  }
}

// If a sensor was reset mid-transfer it can hold SDA low forever. Clocking SCL frees it.
void recoverBus() {
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delay(5);
  if (digitalRead(SDA) == LOW) {
    pinMode(SCL, OUTPUT_OPEN_DRAIN);
    for (int i = 0; i < 9 && digitalRead(SDA) == LOW; i++) {
      digitalWrite(SCL, LOW); delayMicroseconds(5);
      digitalWrite(SCL, HIGH); delayMicroseconds(5);
    }
  }
  if (digitalRead(SDA) == LOW || digitalRead(SCL) == LOW) {
    Serial.printf("I2C line held low (SDA=%d SCL=%d). Check for a wire on the wrong pin or a short to GND.\n",
                  digitalRead(SDA), digitalRead(SCL));
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  recoverBus();
  Wire.begin();
  Wire.setTimeOut(20);  // a stuck bus fails fast instead of hanging the scan
  Wire.setClock(400000);

  sensor.setTimeout(500);
  while (!sensor.init()) {
    Serial.println("VL53L1X not found at 0x29. Check power, GND, SDA (GPIO8) and SCL (GPIO9).");
    diagnoseBus();
    delay(1000);
  }
  Serial.println("VL53L1X found.");
  sensor.setDistanceMode(VL53L1X::Long);
  sensor.setMeasurementTimingBudget(33000);
  sensor.startContinuous(33);
}

void loop() {
  uint16_t mm = sensor.read();  // blocks until the next measurement is ready
  if (sensor.timeoutOccurred()) {
    Serial.println("Timeout waiting for a reading.");
    return;
  }
  Serial.printf("{\"d\":%u,\"status\":%u,\"signal\":%.2f,\"ambient\":%.2f}\n",
                mm,
                (unsigned)sensor.ranging_data.range_status,
                sensor.ranging_data.peak_signal_count_rate_MCPS,
                sensor.ranging_data.ambient_count_rate_MCPS);
}
```

On another board, change the pins with `Wire.begin(SDA_PIN, SCL_PIN)` and use the same pins in `recoverBus()`.

## Reading the output from a laptop (how the tests were run)

pyserial with DTR and RTS held low, so opening the port does not reset the board, then counting the status codes:

```python
import serial, time, json, collections
s = serial.Serial(); s.port = "/dev/ttyACM0"; s.baudrate = 115200; s.timeout = 0.2
s.dtr = False; s.rts = False; s.open()
t0 = time.time(); buf = b""
while time.time() - t0 < 5: buf += s.read(4096)
s.close()
js = [json.loads(l) for l in buf.decode(errors="replace").splitlines() if l.startswith("{") and l.endswith("}")]
c = collections.Counter(j["status"] for j in js)
print(len(js), "samples; status counts:", dict(c))
for st in c:
    sub = [j for j in js if j["status"] == st]
    print(f"status {st}: d {min(j['d'] for j in sub)}-{max(j['d'] for j in sub)} mm")
```

Upload: `pio run -d <project> -t upload --upload-port /dev/ttyACM0`.
