# Third-party licences

This file lists third-party code that Franken-E58 firmware is derived from. Every derived source file also keeps the copyright notices below in its header.

## Pololu VL53L1X library (vl53l1x-arduino)

| Item | Value |
|---|---|
| Project | Pololu VL53L1X library for Arduino, https://github.com/pololu/vl53l1x-arduino |
| Commit used | `8f21d608af00b42dc8776cfdf5ce935b45d2387d` (2022-02-18, "updated readme, license, library.properties") |
| Local copy | `docs/third_party/vl53l1x-arduino/` (ignored by git) |
| Files ported | `VL53L1X.cpp`, `VL53L1X.h` |
| Licence | BSD 3-Clause ("New" or "Revised") |
| Used by | `components/drv_vl53l1x` (Module 4) |

**Licence check (M4).**
- The repository's `LICENSE.txt` grants BSD-3-Clause under the copyright of STMicroelectronics (2017) and Pololu Corporation (2018–2022).
- The two ported files carry no licence header of their own and no other terms, so `LICENSE.txt` covers them.
- The library states that it is derived from ST's VL53L1X API (STSW-IMG007) and that the API's BSD 3-Clause terms apply to it.
- Checked against the local copy at the commit above.

**Statement in LICENSE.txt:**

> Most of the functionality of this library is based on the VL53L1X API provided by ST (STSW-IMG007), and some of the explanatory comments are quoted or paraphrased from the API source code, API user manual (UM2356), and VL53L1X datasheet. Therefore, the license terms for the API source code (BSD 3-clause "New" or "Revised" License) also apply to this derivative work, as specified below.

**Copyright notices:**

```
Copyright (c) 2017, STMicroelectronics
Copyright (c) 2018-2022, Pololu Corporation
All Rights Reserved
```

**Licence text:**

```
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## ST VL53L1X ultra lite driver, via the SparkFun VL53L1X library

| Item | Value |
|---|---|
| Project | SparkFun VL53L1X Arduino Library, https://github.com/sparkfun/SparkFun_VL53L1X_Arduino_Library |
| Commit used | `e88f55d6d82a4a8c519ec90a2660e6ec799b4579` |
| File used | `src/st_src/vl53l1x_class.cpp` (ST code, "@author IMG", 14-December-2018) |
| Code used | The formula in `VL53L1X_SetInterMeasurementInMs()`: `(OSC_CALIBRATE_VAL & 0x3FF) * InterMeasMs * 1.075`, written to `SYSTEM__INTERMEASUREMENT_PERIOD` |
| Licence | BSD 3-Clause, from the file's own header |
| Used by | `components/drv_vl53l1x/drv_vl53l1x.cpp`, `start_continuous()` (M4 D-M4-08) |

**Licence check (M4).**
- The file header grants BSD-3-Clause under "COPYRIGHT(c) 2018 STMicroelectronics". The conditions and disclaimer match the text above, except that condition 3 names STMicroelectronics instead of "the copyright holder".
- Checked against the file downloaded at the commit above. No local copy is kept.

**Copyright notice:**

```
COPYRIGHT(c) 2018 STMicroelectronics
```

## esp_littlefs (joltwallet/littlefs) and the littlefs core (M5)

| Item | Value |
|---|---|
| Project | esp_littlefs, https://github.com/joltwallet/esp_littlefs, from the ESP-IDF component registry as `joltwallet/littlefs` |
| Version used | `==1.22.3`, source commit `bde79e13be971a1f06d5e5f55e6f3e8568fa32b5`, component hash `3af20a3d…` (`dependencies.lock`) |
| Bundled core | littlefs v2.11 (`LFS_VERSION 0x0002000b`, disk format v2.1), `src/littlefs/` |
| How it is used | Pinned managed component, declared in `components/hal_fs/idf_component.yml`, downloaded into `managed_components/` (git-ignored) at the first CMake configure. Not modified |
| Licences | esp_littlefs wrapper: MIT. littlefs core: BSD 3-Clause |
| Used by | `components/hal_fs` (Module 5) |

**Licence check (M5).**
- The registry labels versions from 1.22.3 on as "Custom". The `license.txt` it serves for 1.22.2 and 1.23.1 is byte-identical MIT text, and the `LICENSE` in the downloaded 1.22.3 is the same MIT text.
- The littlefs core keeps its own BSD 3-Clause `LICENSE.md` inside the component.
- Checked against the downloaded 1.22.3 component on 31 October 2026.

**esp_littlefs licence (MIT):**

```
Copyright 2020 Brian Pugh

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

**littlefs licence (BSD 3-Clause):**

```
Copyright (c) 2022, The littlefs authors.  
Copyright (c) 2017, Arm Limited. All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

-  Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
-  Redistributions in binary form must reproduce the above copyright notice, this
   list of conditions and the following disclaimer in the documentation and/or
   other materials provided with the distribution.
-  Neither the name of ARM nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior
   written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

**Obligations for these two:** keep both notices with the source (the component carries them unchanged) and reproduce both in the documentation of any binary release (this file). The littlefs condition 3 names ARM: do not use it to endorse the project.

**Obligations for this project:**
- Keep the notices and licence text above in every derived source file (condition 1).
- Ship this file, or its contents, with any binary release (condition 2).
- Don't use the ST or Pololu names to endorse the project (condition 3).
- Our own code may carry any licence.
