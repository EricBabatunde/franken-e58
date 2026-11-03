# Franken-E58 Phase 1, Module 6: WebSocket and UDP latency harness, IMU loop alongside

**Scope:** the 14-byte control frame on two transports (UDP port 5858 and the `/ws` WebSocket), ping and pong for latency, a lock-free setpoint hand-off read by the Core 1 IMU loop, the laptop latency tool, a phone-browser latency page, and a log analyser for the IMU under traffic (risk R5 re-measurement). Nothing acts on the setpoint: no arming, no motor output. The joystick UI is not part of M6.

Blueprint sections used: §4.2, §4.6 (control frame, latency target), §4.8, Phase 1 M6, risks R3 and R5.

**Status:** M6 passed (bench run 2, 3 November 2026, §10): acceptance 1 to 8 pass; decision 9: WebSocket stays the UI control path, UDP for tools. Run 1 (§9) was spoilt by laptop Wi-Fi scans and a page measurement artefact, both fixed before run 2.

---

## 1. Spec

### Part 0: decisions

| ID | Decision | Reason |
|---|---|---|
| D-M6-01 | One setpoint slot per transport (`comms::setpoint::slot(kUdp/kWs)`); Core 1 reads both and uses the newer by receive time | `SeqlockSlot` is single-writer, and the UDP task and the httpd task both receive control frames. No lock anywhere |
| D-M6-02 | CRC-8: polynomial 0x07, init 0x00, no reflection, no final XOR, over bytes 0 to 12 (CRC-8/SMBUS, check value 0xF4 for `"123456789"`) | Simple, catalogued, detects every single-bit error in a 14-byte frame |
| D-M6-03 | Ping and pong: the pong (type 3) echoes seq, roll, pitch, yaw and flags; throttle carries the device turnaround in µs (receive to send, saturating at 65535); CRC recomputed. Pings never touch the setpoint | Splits RTT into device time and network time |
| D-M6-04 | Clients send a control frame half a period after each ping: 50 Hz each, 100 frames/s per transport (`--no-control` turns them off). Control frames carry neutral sticks, throttle 0, arm bit clear. UDP port 5858 | Exercises the setpoint slot (acceptance 7) at the UI's real rate. Port: our own AP, arbitrary ("E58") |
| D-M6-05 | Host unit test with the laptop's g++ (approved): `g++ -std=c++17 -Wall -Wextra -Werror -Icomponents/comms/include components/comms/control_frame.cpp tools/host_test/test_control_frame.cpp -o .pio/host/test_control_frame && .pio/host/test_control_frame components/comms/test_vectors/control_frame_vectors.txt` | The codec is plain C++ with no IDF headers, so the device source is the tested source |
| D-M6-06 | Shared known-answer vectors `components/comms/test_vectors/control_frame_vectors.txt`, used by the firmware (embedded, `SUMMARY CRC` at boot), the host test, `tools/control_frame.py` and `web/latency.html` (served from LittleFS) | One definition checked by all four implementations. The C++ CRC is bitwise, the Python and JavaScript ones table-driven |
| D-M6-07 | Python tools run in a git-ignored `.venv` made with uv: `uv venv .venv && uv pip install --python .venv/bin/python -r tools/requirements-m6.txt` (websockets 17.4, numpy 2.5.4, matplotlib 3.11.2) | The system Python has none of the packages. User decision |
| D-M6-08 | `/ws` is registered by `http_static::start(extra)` ahead of the `/*` wildcard; the M5 "`/ws` → 404" branch is removed | Wildcard matching takes handlers in registration order |
| D-M6-09 | `platformio.ini` lists the vectors file under `board_build.embed_txtfiles` | PlatformIO cannot find the generated `.S` for a component's `EMBED_TXTFILES` otherwise (the first build failed). Deviation: `platformio.ini` was not in the plan |

`board_config.h`: `kUdpPort = 5858`, `kUdpTaskPriority = 5`, `kSetpointStaleUs = 150 000` (the §4.8 link-silence figure; Core 1 only counts stale reads in M6), `kM6ImuSeconds = 1200`, `FE_M5_TEST 0`, `FE_M6_TEST 1`, the five-way exclusion `#error`, `FE_IMU_ISR_INSTRUMENT` on for M6. `sdkconfig.defaults`: `CONFIG_HTTPD_WS_SUPPORT=y` (name checked in the IDF 5.5.3 Kconfig; `verify_sdkconfig.sh` checks every defaults line, so it needed no change).

### Part 1: firmware (all comms on Core 0)
- **`comms::frame`** (`control_frame.h/.cpp`): encode, decode with the error cause (checked in the order length, magic, CRC, type), pong builder, vector runner. No heap.
- **`comms::link`** (`link.h/.cpp`): the shared receive path. Validates, counts each error cause, writes the transport's setpoint slot on a control frame, builds pongs and records turnaround min/mean/max. A pong sent *to* the device counts as a type error. Per-transport counters are published through a seqlock written only by the transport's task.
- **`comms::udp_server`**: task `udp_rx`, Core 0, priority 5, port 5858. One static 32-byte receive buffer (an over-long datagram reads as more than 14 bytes and is counted as a length error), `recvfrom` and `sendto` on a static socket, no heap per packet. Invalid frames are dropped silently.
- **`comms::ws_server`**: the `/ws` handler (`is_websocket`, `handle_ws_control_frames = false`, so httpd answers WebSocket PING and CLOSE itself). The handler runs in the **httpd task, pinned to Core 0** (`core_id = 0`, checked at run time by `SUMMARY TASKS`). Binary, final, 14-byte frames only; anything else, or a frame that fails validation, returns an error and httpd closes the session (`httpd_parse.c:813`, `httpd_sess.c:428`). A GET to `/ws` without the upgrade reaches the handler too (`httpd_uri.c:325` only performs the handshake when it detected one), so the handler checks `httpd_ws_get_fd_info()` and answers 400. Pongs go out with `httpd_ws_send_frame()`, synchronously. `TCP_NODELAY` comes from the M5 `open_fn`.
- **Setpoint record** (`setpoint.h`): frame fields, `t_rx_us` from `esp_timer_get_time()` (the same timebase on both cores), transport, and an FNV-1a check word so the consumer can prove no torn record reaches it.
- **`seqlock.h`**: a backward-compatible `read(out, max_tries, &tries_used)` overload for counting retries.
- **Core 1 reader** (`src/m6_latency_test.cpp`, `sample_hook`): `m3_imu_test` gained `m3_set_sample_hook()`, called after every successful Phase B read with the window number (null in M3 to M5). The hook reads both slots, counts retries, give-ups and check-word mismatches, records the age of the newest setpoint (live ages below 1 s; stale above 150 ms counted), and times itself with `esp_cpu_get_cycle_count()`. It publishes its window through a seqlock at each window change and every 1000 samples. No logging, allocation or shared lock.
- **Bench test** (`src/m6_latency_test.cpp`, task `m6_test`, Core 0): banner with the plan, `SUMMARY CRC`, FS, AP, HTTP+WS, UDP, then one M3 Phase B run of 1200 s with the hook, `SUMMARY TASKS` (adds `udp_rx` and `httpd` on Core 0, `imu` on Core 1). One `m6 wNN` line per Core 1 window, numbered like the `m3: B wNN` line: class (idle, udp, ws, mixed), frames, pings, controls, each error cause and mean turnaround per transport, WebSocket sessions, the hook figures, station RSSI (`esp_wifi_ap_get_sta_list`), internal heap. At the end: `SUMMARY M6-IMU`, `SUMMARY SETPOINT`, `SUMMARY COMMS UDP/WS`, `SUMMARY HEAP` and a recap; then an `idle` comms line every 10 s.

### Part 2: tools and page
- **`tools/latency_test.py`** (`.venv/bin/python`): `--transport udp|ws`, `--rate` (50), `--count` (10000), `--host` (192.168.4.1), `--port` (5858), `--out` (default `logs/m6/<transport>-<time>`), `--no-control`. Sends on absolute deadlines from `perf_counter_ns` (asyncio sleep to within 3 ms, then yield-spin) and records each send's lateness. Matches pongs by seq; 200 ms timeout; counts loss (including late pongs), out-of-order, unknown, invalid and echo errors. Reports RTT/2, RTT, device turnaround and the network part (RTT − turnaround) as min/mean/p50/p95/p99/p99.9/max. Writes CSV (every ping), JSON (summary) and a PNG histogram (0.5 ms bins, log y, target and measured p95/p99 lines).
- **`--selftest`**: the same code against a local fake device on 127.0.0.1 (UDP and WebSocket, same validation and pong rules), 500 pings each, plus the vectors, a bad-CRC datagram (dropped and counted) and a wrong-length WebSocket frame (session closed). Reports the host RTT floor and the sender lateness.
- **`web/latency.html`** (gzipped into `data/` by `tools/build_web.py`, with the vectors): WebSocket to `/ws`, ping + control at 50 Hz on drift-corrected `setTimeout` deadlines, RTT from `performance.now()`. Live count, loss, out-of-order, one-way p50/p95/p99/max, turnaround, timer lateness and a text histogram, all kept as running sums and a fixed 0.05 ms histogram so a refresh costs the same throughout the run; exact percentiles are computed once at the end. Stops at the chosen count (default 10000), offers a JSON download (summary, every RTT, every ping's send time from the first deadline, every event's lateness, and the slowest refresh during the run). Stops sending when the page is hidden and shows that state. Checks the vectors on load.
- **`tools/analyse_m6.py`** (standard library): pairs `m3: B wNN` and `m6: wNN` lines from the last M6 boot in a log and prints, per class (idle, udp, ws#1 laptop, ws#2 phone, mixed): window count, sd max, jitter max, wake avg/max, burst average range and max, missed/timeouts/I2C errors, setpoint age max, retry max, torn, stale and hook cost, with the acceptance 6 verdict and R5 flags against M3 and M5. Stale reads show "-" for idle and mixed windows, where the last setpoint is simply ageing.

## 2. Runtime flash writes
None added. The M5 table still applies (`docs/M5_package.md` §1).

## 3. Acceptance criteria
1. Clean build; `tools/verify_sdkconfig.sh` passes (including `CONFIG_HTTPD_WS_SUPPORT=y`); the host unit test and `latency_test.py --selftest` pass.
2. UDP from the laptop: 10 000 pings at 50 Hz, one-way (RTT/2) p95 < 15 ms and p99 < 20 ms. Loss and out-of-order counts reported (flag above 0.5 %).
3. WebSocket from the laptop: the same.
4. WebSocket from the phone browser: the same, with the loss count and what the screen did.
5. Host RTT floor and the sender's scheduling jitter reported for the laptop runs.
6. IMU alongside: zero missed samples, zero I2C errors, interval sd < 2 µs, jitter < 20 µs, wake max < 50 µs in the traffic windows. Burst average and maximum reported against M3 (450.4 µs) and M5 (605 to 633 µs under page serving) as the R5 re-measurement.
7. Setpoint slot: zero torn reads reach the consumer. Maximum retries and maximum setpoint age seen on Core 1 reported.
8. Heap within 2 KB of the idle level after more than 10 000 frames per transport.
9. Decision recorded: if the phone-browser WebSocket meets both targets, keep it as the UI control path and UDP for tools; if only UDP meets them, propose the native UDP client as the primary path (R3), with the numbers.

## 4. Bench procedure
1. **Flags:** `FE_M6_TEST` 1, the other test flags 0. IMU wired as for M3.
2. **Build (laptop):** `python3 tools/build_web.py`, `pio run`, `tools/verify_sdkconfig.sh`, `pio run -t buildfs`. Host test (D-M6-05) and `.venv/bin/python tools/latency_test.py --selftest`.
3. **Flash (user):** `pio run -t upload`, then `pio run -t uploadfs` (the page and the vectors are new files).
4. **Monitor** from the project folder (the log goes to `logs/`).
5. **Laptop Wi-Fi power save and scans:** record power save with `iw dev <wlan> get power_save` and, for the laptop runs, turn it off (`sudo iw dev <wlan> set power_save off`; it resets on reconnect, so check again after joining the AP). Turn off GNOME Location Services (Settings → Privacy), which made the laptop scan every 15 s in run 1. During each laptop run keep `iw event -t | grep -i scan` open in a second terminal; any scan line spoils the run.
6. **Idle baseline:** join the laptop when the banner appears; no traffic for the first 60 s (six idle windows; the heap reference is taken in the last idle window with a station before the first traffic).
7. **UDP:** `.venv/bin/python tools/latency_test.py --transport udp` (about 205 s).
8. **WebSocket, laptop:** `.venv/bin/python tools/latency_test.py --transport ws`.
9. **Phone:** disconnect the laptop (one station maximum), join the phone, open `http://192.168.4.1/latency.html`, check "Frame vectors ... PASS", raise the screen timeout (no wake lock over http, §7), press Start, keep the page in front. Download the JSON at the end. Note what the screen did.
10. **Quiet:** leave the link idle until the recap (the IMU run is 1200 s from boot; the firmware then waits for 10 s without frames before the heap check).
11. **Analyse:** `python3 tools/analyse_m6.py` (newest log).
12. **Copy back:** the recap block, the analyser table, both laptop summaries and the phone JSON summary fields, the laptop power-save setting, what the phone screen did, and any warning or error.

## 5. Report-only investigations

### 1. esp_http_server WebSocket support in IDF 5.5.3
- **Frame size:** no fixed limit. `httpd_ws_recv_frame(req, &f, 0)` reads the header and returns the length (7-, 16- or 64-bit form); a second call copies the payload into the caller's buffer and returns `ESP_ERR_INVALID_SIZE` if it is larger than `max_len` (`httpd_ws.c:354`). We check the length first and use a 14-byte static buffer.
- **Fragmentation:** not reassembled. Continuation frames reach the handler as `HTTPD_WS_TYPE_CONTINUE` with the `final` flag; we close the session on anything but a final binary frame.
- **Where handlers run:** in the single httpd task (here Core 0, priority 5), one request at a time. Data frames on an upgraded socket go straight to the stored WebSocket handler (`httpd_parse.c:789`); PING is answered and CLOSE acknowledged by httpd itself when `handle_ws_control_frames` is false (`httpd_ws.c:502`).
- **Blocking:** `httpd_ws_send_frame()` is synchronous: it writes from the httpd task and can block for up to `send_wait_timeout` (5 s default) if the client stops reading. Receives block up to `recv_wait_timeout` (5 s) if a client sends a header and then stalls. Either would stall all HTTP serving, not Core 1. `httpd_ws_send_frame_async()` queues the send onto the httpd task instead (not needed for pongs).
- **A client that vanishes without closing:** nothing is detected while the device has nothing to send. TCP keep-alive is off by default (`keep_alive_enable = false`), so the session stays open until a send fails, the LRU purge reclaims it when all 7 sockets are in use, or a new frame errors. For flight, link loss is detected by setpoint age (150 ms), not by the socket. Candidate for M7: `keep_alive_enable` with short idle/interval settings.

### 2. Wi-Fi settings that influence latency
| Setting | Value | Effect |
|---|---|---|
| Beacon interval | 100 TU (102.4 ms), set explicitly | A dozing station is told of buffered unicast frames only in beacons: with client power save, downlink delay can approach one beacon interval or more |
| DTIM period | not set: the zeroed `wifi_ap_config_t` holds 0; IDF documents range 1 to 10, default 1 (verify against IDF docs) | Affects buffered broadcast and multicast only. Proposed: set 1 explicitly in M7 (B-M6-08) |
| AP power save | `esp_wifi_set_ps(WIFI_PS_NONE)` | The soft-AP itself does not sleep |
| Channel, bandwidth | 1, HT20 | Fixed; fewer retries than HT40 in a busy band |
| A-MPDU | TX and RX enabled (defaults) | Aggregation can hold frames briefly; left on, re-check if the tails are poor |
| Client power save | outside our control | The laptop's can be turned off for the bench (§4 step 5). Phones usually keep it on; this is the main risk for acceptance 4 |

### 3. Cost of the WebSocket support
| Build (31 Oct / 3 Nov 2026) | App image | IRAM text | DRAM `.data` | DRAM `.bss` |
|---|---|---|---|---|
| M5 code and config | 856 896 B | 94 247 B | 24 744 B | 26 752 B |
| + `CONFIG_HTTPD_WS_SUPPORT=y` only | 863 232 B (+6 336: text +3 920, rodata +2 416) | +0 | +0 | +0 |
| Full M6 firmware | 873 440 B (27.8 % of 3 MB; +16 544 over M5) | +0 | 25 640 B (+896) | 30 024 B (+3 272) |

The option costs flash only. Runtime heap: the WebSocket state lives in httpd's session table, allocated when the server starts; `SUMMARY HTTP+WS` and `SUMMARY UDP` print the internal heap before and after each start (bench figures to be recorded).

### 4. Can the UDP receive path drop packets?
- lwIP queues each datagram on the socket's receive mailbox (`CONFIG_LWIP_UDP_RECVMBOX_SIZE` = 6). If the mailbox is full, `recv_udp()` frees the datagram silently (`api_msg.c:283`). `LWIP_SO_RCVBUF` is off, so the mailbox depth is the only limit, and `CONFIG_LWIP_STATS` is off, so lwIP keeps no drop counter.
- Upstream: the tcpip mailbox (32) and the Wi-Fi RX buffers (10 static, 32 dynamic).
- At 100 frames/s, six slots hold 60 ms of traffic; `udp_rx` (priority 5) would have to be starved that long by higher-priority Core 0 work (wifi 23, tiT 18) for a drop.
- **What the counters would show:** a drop never reaches `recvfrom`, so no firmware error counter moves. It appears as client loss with the firmware's `udp rx` lower than the frames the client sent (pings + controls). An invalid frame, by contrast, moves an error counter.

## 6. Proposed blueprint diffs (not applied; revision 2.9 when approved)

| ID | Section | Old | New | Reason |
|---|---|---|---|---|
| B-M6-01 | §4.2 | One setpoint slot (atomic, timestamped) | One seqlock setpoint slot per transport; Core 1 uses the newer | Single-writer seqlock, two receiving tasks (D-M6-01) |
| B-M6-02 | §4.6 control frame | "crc8 over bytes 0 to 12" | CRC-8 poly 0x07, init 0x00, no reflection, no final XOR (check 0xF4), shared vectors file | D-M6-02 |
| B-M6-03 | §4.6 | Types 2 ping, 3 pong (no semantics) | Pong echoes seq, roll, pitch, yaw, flags; throttle = device turnaround µs, saturating | D-M6-03 |
| B-M6-04 | §4.6 | UDP port unspecified | UDP port 5858 | D-M6-04 |
| B-M6-05 | §4.6 | WebSocket `/ws` | Binary 14-byte frames only; any other frame closes the session; plain GET 400; handler in the httpd task on Core 0 | M6 implementation |
| B-M6-06 | §4.9 | none | `CONFIG_HTTPD_WS_SUPPORT=y` | Required for `/ws` |
| B-M6-07 | §4.6 joystick UI | (screen wake not addressed) | No screen wake lock over plain http (browsers require a secure context); hidden page stops sending; the user sets the screen timeout | Browser rule |
| B-M6-08 | §4.6 Wi-Fi AP | DTIM not stated | Set `dtim_period = 1` explicitly (now 0 in a zeroed config, IDF default 1) | Investigation 2 |
| B-M6-09 | §5 Phase 1, M6 row | p95/p99 targets, histograms | Add: IMU alongside (acceptance 6), setpoint slot (acceptance 7), heap (acceptance 8) | M6 scope as specified |
| B-M6-10 | §4.2 setpoint hand-off | Seqlock writer runs preemptible | The Core 0 writer wraps each slot write in a short Core 0 critical section (interrupts off for the copy, about a microsecond; nothing shared with Core 1) | Bench run 1: 6 Core 1 reads gave up after 8 tries, most likely a writer preempted mid-write (§9). No torn data reached Core 1, but a give-up costs a loop iteration its fresh setpoint |
| B-M6-11 | §4.3 Core 1 loop, R5 | Control path IRAM_ATTR (rule); M6 hook in flash | Put the Core 1 setpoint read (hook, `SeqlockSlot::read`, check word) in IRAM, or measure `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` (verify against IDF docs) | Bench run 1: hook average 3 µs idle, 7.7 µs under UDP, 21.6 µs under WebSocket (max 95 µs); I2C burst average 451 µs idle, 469 to 476 µs UDP, 517 to 526 µs WebSocket. Both fit Core 0's larger code footprint under WebSocket evicting Core 1's flash code from the shared 16 KB I-cache (R5 mechanism) |
| B-M6-12 | §4.6 ground station | (not addressed) | The laptop ground station runs with Wi-Fi power save off and with background scanning sources (location services) off; a scan on the controlling client takes it off channel for over 1 s | Bench run 1: scans every 15 s (4.4 s every 75 s) gave 50 to 90 ms latency bursts and setpoint ages up to 162 ms, past the 150 ms link-loss cutoff |

## 7. Spec items that do not fit ESP-IDF 5.5.3 or the browsers as written
- **Screen wake lock:** `navigator.wakeLock` exists only in a secure context (HTTPS or localhost). On `http://192.168.4.1` no browser offers it; the page says so and the bench procedure asks for a longer screen timeout.
- **One setpoint slot from two transports:** not possible with a single-writer seqlock without a writer lock (D-M6-01).
- **"A plain GET to /ws returns 400":** not automatic in 5.5.3; the handler is called for a plain GET and must answer 400 itself.
- **Embedded files:** need a `platformio.ini` entry under PlatformIO (D-M6-09).
- **Send blocking:** the synchronous pong send can block the httpd task for up to 5 s on a stalled client (investigation 1); harmless to Core 1, relevant to M7's failsafe design only through the socket staying open.

## 8. Development results (3 November 2026, no hardware)
- **Build:** clean, 0 warnings; `verify_sdkconfig.sh` 25/25; `buildfs` OK (`index.html.gz` 540 B, `latency.html.gz` 5 363 B, 6 166 B after the run 1 fix, `control_frame_vectors.txt.gz` 918 B, `test.bin.gz` 102 453 B).
- **Host unit test:** 4 043 checks, 0 failed (catalogue check value, 40 vector checks, 2 000 random round trips, every single-bit error rejected, pong saturation). A deliberately corrupted vectors file fails it.
- **Python codec:** 40 vector checks pass. **Page codec** (run under Node 24): 28 vector lines pass; end-to-end against the fake device: 300/300 pongs, 300 control frames, sending stops on a hidden page.
- **Self-test:** 13/13 checks (the 13th, added after the first bench run, aborts the WebSocket mid-run and checks that the tool stops sending and reports the lost link). Host RTT floor (loopback, tool plus fake device), min/p50/p99: UDP 0.07/0.29/0.44 ms, WebSocket 0.18/0.87/1.08 ms. Sender lateness mean/max: UDP 25/52 µs, WebSocket 27/126 µs.

## 9. Bench run 1 (3 November 2026, `logs/device-monitor-261103-101912.log`)
Laptop power save off for both laptop runs. The page and laptop tool were the versions before the fixes below.

| # | Result | Figures |
|---|---|---|
| 2 UDP laptop | FAIL (laptop scans) | 9 712/10 000 received, loss 2.88 % (flag); one-way p50/p95/p99/max 1.23/5.12/51.18/68.27 ms; turnaround mean 18 µs; sender lateness mean 24 µs |
| 3 WS laptop | FAIL (laptop scans) | 9 997/10 000, loss 0.03 %; one-way 3.13/32.86/58.30/94.01 ms; turnaround mean 309 µs; sender lateness mean 25 µs |
| 4 WS phone | FAIL (page artefact) | Android 10, Chrome 155: 10 000/10 000, loss 0; one-way 3.90/5.35/23.25/59.5 ms; screen stayed on (timeout set to never; no wake lock over http) |
| 6 IMU | PASS | Every class: 0 missed/timeouts/I2C errors; sd max 0.60 µs; jitter max 14.7 µs; wake max 42 µs (ws#2). Burst average: idle 451 to 466, udp 469 to 476, ws 517 to 526 µs (M3 450.4, M5 605 to 633); burst max 776 µs |
| 7 Setpoint | PASS | 0 torn in 1 198 645 reads; max retries in one read 7; 6 reads given up; max age 162 ms (laptop scans); 16 stale reads |
| 8 Heap | PASS | −1 200 B after 19 424 UDP and 40 000 WS frames; min free 181 255 B |
| 9 | Open | Needs run 2 |

- **Laptop scans:** in both laptop runs, loss and latency came in bursts every 15.0 s (about 1.1 s long) with a longer one (about 4.4 s) every 75 s. Outside the bursts, UDP p99 was 8.9 ms and WebSocket p99 16.8 ms. `iw event` then showed scans every 15 s; turning off GNOME Location Services stopped them. The phone on the same AP showed no such pattern, so the device was not the cause.
- **Page artefact:** the page re-sorted every result on each 500 ms refresh. Late in the run that blocked the phone's main thread for up to about 60 ms, delaying both pong timestamps and sends: the phone's outliers were single pings at a fixed 0.5 s phase growing from 21 to 59 ms, the device's setpoint-age maximum in those windows rose steadily from 32 to 134 ms, and the page's own timer lateness averaged 9.9 ms. Fixed (§1 Part 2); under Node a refresh now costs about 0.3 ms with a full histogram, and the live percentiles agree with the exact ones to within one 0.05 ms bin.
- **WebSocket against UDP:** device turnaround mean 309 µs against 18 µs (httpd path), and laptop one-way p50 3.13 against 1.23 ms.
- **First attempt (09:42 log):** the device browned out mid-run (`E BOD`); the 09:39 log shows 8 more brownouts at Wi-Fi start-up. Run 1 had no brownout and no reset after boot. `latency_test.py` now stops and reports a lost link instead of printing a traceback per send (self-test check 13).

## 10. Bench run 2 (3 November 2026): M6 passed
Logs: `logs/device-monitor-261103-150125.log` (boot to w93) and `logs/device-monitor-261103-151835.log` (w99 to the recap; the serial monitor stopped and was restarted, so idle windows w94 to w98 are missing). The analyser ran on the two joined. Laptop: Wi-Fi power save off, Location Services off, GNOME Settings closed, `iw event` showed no scans. Phone: Android 10, Chrome 155, the fixed page; no warnings on the page; screen timeout set to never (no wake lock over http).

| # | Result | Figures |
|---|---|---|
| 1 | PASS | §8 |
| 2 UDP laptop | PASS | 10 000/10 000, loss 0, out of order 0; one-way min/p50/p95/p99/p99.9/max 0.96/1.21/2.00/2.59/11.20/15.59 ms; turnaround mean 18 µs (max 1 718); network part p99 5.16 ms RTT |
| 3 WS laptop | PASS | 10 000/10 000, loss 0, out of order 0; one-way 2.25/3.03/3.86/5.26/17.09/18.23 ms; turnaround mean 317 µs (max 5 777) |
| 4 WS phone | PASS | 10 000 sent, 9 968 received, 32 lost (0.32 %, under the 0.5 % flag; all late beyond 200 ms), out of order 0; one-way 2.55/3.95/4.90/8.25/74.4/98.3 ms; turnaround mean 278 µs. Page refresh max 12.4 ms, timer lateness mean 2.1 ms, max 22.3 ms (first event) |
| 5 | Reported | Host RTT floor (self-test, loopback) min/p50/p99: UDP 0.07/0.29/0.44 ms, WS 0.18/0.87/1.08 ms. Sender lateness mean/p99/max: UDP ping 27/46/185 µs, WS ping 28/46/877 µs |
| 6 | PASS | See table below. Whole run: sd 0.51 µs, jitter 13.6 µs, wake max 19 µs, burst avg/max 480.2/778 µs, 0 missed/timeouts/I2C errors in 1 199 627 samples |
| 7 | PASS | 0 torn in 1 198 630 reads; max retries in one read 7; 3 reads given up. Setpoint age max during traffic: laptop 48 ms (UDP and WS), phone 61 ms apart from one 324 ms gap (phone stall, below); the firmware's whole-run 999.7 ms and 4 253 stale reads come from the last window of each run, classed as traffic but ending in silence |
| 8 | PASS | Internal heap change +28 B after 20 000 UDP and 40 000 WS frames (idle 211 387 B, after 211 415 B). Minimum free 146 831 B (below) |
| 9 | Decided | The phone-browser WebSocket meets both targets: keep it as the UI control path and UDP for tools (R3 unchanged) |

`tools/analyse_m6.py` on the joined logs:

| Class | Win | sd max | jitter max | wake avg / max | burst avg | burst max | miss/to/i2c | retry max | hook avg / max µs |
|---|---|---|---|---|---|---|---|---|---|
| idle | 51 | 0.50 | 4.6 | 7.0 / 18 | 451 to 459 | 762 | 0/0/0 | 0 | 3.26 / 90.71 |
| udp | 20 | 0.50 | 9.4 | 7.2 / 19 | 470 to 480 | 756 | 0/0/0 | 1 | 7.97 / 96.95 |
| ws#1 laptop | 20 | 0.60 | 13.6 | 7.4 / 18 | 516 to 534 | 776 | 0/0/0 | 7 | 22.27 / 94.86 |
| ws#2 phone | 20 | 0.60 | 12.6 | 7.4 / 19 | 514 to 525 | 778 | 0/0/0 | 7 | 21.48 / 96.93 |
| mixed | 3 | 0.50 | 4.6 | 7.1 / 15 | 457 to 464 | 740 | 0/0/0 | 1 | 4.81 / 92.78 |

Acceptance 6 PASS in every class. R5 flags against M3 in every traffic class (wake, jitter, burst): the WebSocket burst average is about 65 µs above idle and the hook cost about 19 µs above idle, as in run 1 (B-M6-11). Both stay below the M5 page-serving figures (burst 605 to 633 µs).

Findings:
- **Phone stall:** one event at t ≈ 85 s into the phone run: all 32 lost pings and 23 of the slow ones fall within 84.96 to 86.02 s. The page sent on time throughout (worst lateness 22 ms), so the stall was in the network or the phone's Wi-Fi (most likely a phone-side scan or power-save period, outside our control). On the device it appears as a 324 ms setpoint gap in w64 (227 stale reads) and the heap minimum dropping from 177 159 to 146 831 B while TCP buffered and retransmitted; the heap recovered at once. In flight this would trip the 150 ms link-loss cutoff once in 200 s: input for M7's failsafe and for the phone as controller (R3).
- **Seqlock give-ups:** 3 (run 1: 6), each after 8 tries, never with a torn record (B-M6-10).
- **End-of-run windows:** the classifier counts a window as traffic with 400 or more pings even if the run stops part-way, so the firmware's `SUMMARY SETPOINT` age maximum and stale count include up to a second of post-run silence. Reading the per-window lines removes it; a classifier fix is not needed for M6.

## 11. Files
```
components/comms/   include/control_frame.h, control_frame.cpp, include/link.h, link.cpp,
                    include/setpoint.h, include/udp_server.h, udp_server.cpp,
                    include/ws_server.h, ws_server.cpp, test_vectors/control_frame_vectors.txt
src/m6_latency_test.h, src/m6_latency_test.cpp   (bench test, deleted before Module 7)
web/latency.html
tools/control_frame.py, tools/latency_test.py, tools/analyse_m6.py, tools/requirements-m6.txt,
tools/host_test/test_control_frame.cpp
docs/M6_package.md
```
Also changed: `board_config.h`, `sdkconfig.defaults`, `platformio.ini` (D-M6-09), `components/comms/CMakeLists.txt`, `http_static.h/.cpp` (extra handler, field-by-field `httpd_uri_t`), `components/seqlock/include/seqlock.h` (retry-counting read), `src/m3_imu_test.h/.cpp` (sample hook), `src/main.cpp`, `src/CMakeLists.txt`, `tools/build_web.py` (vectors into `data/`), `CLAUDE.md`.
