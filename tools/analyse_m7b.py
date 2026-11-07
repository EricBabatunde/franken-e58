#!/usr/bin/env python3
"""M7b log analysis: watchdog cuts, hangs and resets, boots after a reset while armed, IMU
recovery, the task table, the critical-section A/B and the armed soak (acceptance 14).

Reads one or more PlatformIO monitor logs (joined in the order given; default: the newest
logs/device-monitor-*.log) and uses every M7b boot in them. Standard library only:
    python3 tools/analyse_m7b.py [logfile ...]
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BANNER = "M7b bench firmware: no motors may be connected"
WDT_PERIOD_US = 500          # board::kWdtPeriodUs
HB_US, IMU_US = 5000, 3000   # board::kWdtHeartbeatStallUs, board::kImuMissingCutUs
FORCE_MAX_US = 50.0          # acceptance 2: decision -> force-low
SOAK_MIN_S = 600             # acceptance 14

RE_RESET = re.compile(r"m7b: RESET: reason (\w+)(.*?)\| brownout resets this power-on (\d+) \| controlled restarts this power-on (\d+)/(\d+)")
RE_HANG = re.compile(r"m7b: HANG: key (\S) \| hang -> reset ([-\d.]+) ms .*?\| reset reason (\w+)")
RE_FLIGHT = re.compile(r"m7b: FLIGHT: (PASS|FAIL) \| bus A (\S+) \(lines (.+?), (\d+) clocks\) \| IMU init (OK|FAILED) after (\d+) attempt")
RE_ATTEMPT_FAIL = re.compile(r"m7b: FLIGHT: IMU init attempt (\d+) failed at ([^|]+?) \| error (\S+)")
RE_WDT = re.compile(r"m7b: WDT: (PASS|FAIL) \((\w+)\) \| GPTimer alarm every (\d+) us .*? level (-?\d+) on core (-?\d+)")
RE_TASKS = re.compile(r"m7b: TASKS: (PASS|FAIL)")
RE_TASK = re.compile(r"m7b: TASK (\S+(?: \S+)?)\s+core (\S+)\s+prio\s+(\d+/\s*\d+)\s+stack-min-free\s+(\d+) \| (.*)$")
RE_CONFIG = re.compile(r"m7b: CONFIG: (.*)$")
RE_EV = re.compile(r"m7b: EV #(\d+) t=(-?\d+) us (.*)$")
RE_WDCUT = re.compile(r"CUT (wd_heartbeat|wd_imu_missing) \| last (?:heartbeat|data-ready) -> watchdog decision (\d+) us \| force-low written ([\d.]+) us")
RE_REC = re.compile(r"m7b: REC #(\d+) t=(-?\d+) us attempt (\d+)/(\d+) state (\w+) \| data-ready missing ([\d.]+) ms \| bus_reset (\S+) \| IMU init (OK|FAILED).*\| ([\d.]+) ms$")
RE_WDTWIN = re.compile(r"m7b: w(\d+) wdt \| alarms (\d+) \(expected (\d+)\) \| ISR body min/avg/max ([\d.]+)/([\d.]+)/([\d.]+) us \| alarm -> ISR entry avg/max ([\d.]+)/([\d.]+) us")
RE_B = re.compile(r"m7b: B\s+w(\d+) .*?\| missed (\d+) timeouts (\d+) i2c_err (\d+)")
RE_SUM_TR = re.compile(r"m7b: SUMMARY (udp|ws): rx (\d+) controls (\d+) pings (\d+) .*?critical section (ON|OFF)\) avg/max ([\d.]+)/([\d.]+) us, seqlock write alone avg/max ([\d.]+)/([\d.]+) us")
RE_SUM_RD = re.compile(r"m7b: SUMMARY Core 1 setpoint reads: giveups (\d+) .*?torn (\d+)")
RE_ST = re.compile(r"m7b: ST (\w+)")
RE_TS = re.compile(r"^[IWE] \((\d+)\)")
RE_KEY = re.compile(r"m7b: KEY (\w)")


def load(paths):
    lines = []
    for p in paths:
        lines += pathlib.Path(p).read_text(errors="replace").splitlines()
    starts = [i for i, l in enumerate(lines) if BANNER in l]
    if not starts:
        sys.exit("no M7b boot found")
    firsts = [i for i in starts if i - 1 not in starts]
    return [lines[i:j] for i, j in zip(firsts, firsts[1:] + [len(lines)])]


def stat(v, f="%.1f"):
    if not v:
        return "none"
    return ("min " + f + " max " + f + " mean " + f) % (min(v), max(v), sum(v) / len(v))


def main():
    if len(sys.argv) > 1:
        paths = sys.argv[1:]
    else:
        logs = sorted((ROOT / "logs").glob("device-monitor-*.log"))
        if not logs:
            sys.exit("no logs/device-monitor-*.log found")
        paths = [logs[-1]]
    boots = load(paths)
    print(f"===== M7b analysis: {', '.join(pathlib.Path(p).name for p in paths)} | M7b boots: {len(boots)} =====")

    wd = {"wd_heartbeat": [], "wd_imu_missing": []}
    entry_max = []
    isr_body = []
    alarms_short = 0
    hangs = []
    boot_rows = []
    recs = []
    crit = {}
    tasks_seen = None
    soak = []
    prev_end_state = None
    for b, ls in enumerate(boots, 1):
        row = dict(boot=b, reset="?", flight="?", attempts=0, fail_steps=[], wdt="?", tasks="?",
                   reached_disarmed=False, prev_state=prev_end_state, brownouts=0, restarts=0)
        state = "BOOT"
        armed_t = None
        armed_cuts_wd = 0
        tasks = []
        for l in ls:
            m = RE_TS.match(l)
            ts = int(m.group(1)) if m else None
            if (m := RE_RESET.search(l)):
                row.update(reset=m.group(1), brownouts=int(m.group(3)), restarts=int(m.group(4)))
            elif (m := RE_HANG.search(l)):
                hangs.append((b, m.group(1), float(m.group(2)), m.group(3)))
            elif (m := RE_FLIGHT.search(l)):
                row.update(flight=m.group(1), attempts=int(m.group(6)), lines=m.group(3))
            elif (m := RE_ATTEMPT_FAIL.search(l)):
                row["fail_steps"].append(f"{m.group(2).strip()} ({m.group(3)})")
            elif (m := RE_WDT.search(l)):
                row["wdt"] = f"{m.group(1)} core {m.group(5)} level {m.group(4)} every {m.group(3)} us"
            elif (m := RE_TASKS.search(l)):
                row["tasks"] = m.group(1)
                if tasks_seen is None:
                    tasks_seen = (b, tasks[:], m.group(1))
            elif (m := RE_TASK.search(l)):
                tasks.append((m.group(1), m.group(2), m.group(3), m.group(4), m.group(5).strip()))
            elif (m := RE_WDTWIN.search(l)):
                cnt, exp = int(m.group(2)), int(m.group(3))
                if cnt + 2 < exp:
                    alarms_short += 1
                isr_body.append(float(m.group(6)))
                entry_max.append(float(m.group(8)))
            elif (m := RE_REC.search(l)):
                recs.append((b, int(m.group(3)), m.group(5), float(m.group(6)), m.group(7), m.group(8), float(m.group(9)), ts))
            elif (m := RE_SUM_TR.search(l)):
                key = m.group(5)
                c = crit.setdefault(key, {"controls": {}, "giveups": None, "torn": None, "inside_max": 0.0})
                c["controls"][m.group(1)] = int(m.group(3))
                c["inside_max"] = max(c["inside_max"], float(m.group(9)))
                c["boot"] = b
            elif (m := RE_SUM_RD.search(l)):
                for c in crit.values():
                    if c.get("boot") == b:
                        c["giveups"], c["torn"] = int(m.group(1)), int(m.group(2))
            elif (m := RE_EV.search(l)):
                text = m.group(3)
                t_us = int(m.group(2))
                if (mw := RE_WDCUT.search(text)):
                    wd[mw.group(1)].append((int(mw.group(2)), float(mw.group(3))))
                if "STATE SAFE -> DISARMED" in text:
                    row["reached_disarmed"] = True
                if "-> ARMED" in text and "REFUSED" not in text:
                    state = "ARMED"
                    armed_t = t_us
                    armed_cuts_wd = 0
                elif text.startswith("CUT ") or text.startswith("DISARM ("):
                    if armed_t is not None:
                        soak.append((b, (t_us - armed_t) / 1e6, text.split("|")[0].strip(), False))
                    armed_t = None
                    state = "DISARMED"
                elif "STATE" in text and "-> " in text:
                    state = text.split("-> ")[1].split()[0]
            elif (m := RE_ST.search(l)):
                state = m.group(1)
        if armed_t is not None:
            soak.append((b, None, "still ARMED at the end of the boot (reset or end of log)", True))
        prev_end_state = state
        boot_rows.append(row)

    # ---- Watchdog cuts (acceptance 2 and 3).
    lat = max(entry_max) if entry_max else 0.0
    for name, thr, acc in (("wd_heartbeat", HB_US, 2), ("wd_imu_missing", IMU_US, 3)):
        v = wd[name]
        ages = [a for a, _ in v]
        forces = [f for _, f in v]
        hi = thr + WDT_PERIOD_US + lat
        ok = len(v) >= 20 and all(thr < a <= hi for a in ages) and all(f < FORCE_MAX_US for f in forces)
        print(f"{name}: {len(v)} cuts | decision age us {stat(ages, '%.0f')} | force-low us {stat(forces, '%.2f')}"
              f" | acceptance {acc} (>= 20, {thr} < age <= {hi:.1f} = {thr} + {WDT_PERIOD_US} + ISR entry max"
              f" {lat:.1f}, force < {FORCE_MAX_US:.0f} us): {'PASS' if ok else 'FAIL' if v else 'no data'}")
    print(f"watchdog ISR: body max {max(isr_body) if isr_body else 0:.2f} us, alarm -> entry max {lat:.2f} us,"
          f" windows with missing alarms {alarms_short}")

    # ---- Hangs and resets (acceptance 4).
    want = {"H": "INT_WDT", "T": "TASK_WDT", "X": "PANIC"}
    for b, key, ms, why in hangs:
        nxt = boot_rows[b - 1]
        print(f"hang key {key}: hang -> reset {ms:.1f} ms | reset reason {why} (expected {want.get(key, '?')}:"
              f" {'yes' if why == want.get(key) else 'NO'}) | next boot DISARMED: {'yes' if nxt['reached_disarmed'] else 'NO'}")
    if not hangs:
        print("hangs (H, T, X): none in these logs")

    # ---- Boots, and boots after a reset while armed (acceptance 5).
    print(f"{'boot':>4} {'reset':9} {'prev end':9} {'flight':6} {'att':>3} {'lines':30} {'wdt':28} {'tasks':5} "
          f"{'DISARMED':8} {'brown':>5} {'restarts':>8} failures")
    after_armed = []
    for r in boot_rows:
        print(f"{r['boot']:4d} {r['reset']:9} {str(r['prev_state']):9} {r['flight']:6} {r['attempts']:3d} "
              f"{r.get('lines', '?'):30} {r['wdt']:28} {r['tasks']:5} {'yes' if r['reached_disarmed'] else 'no':8} "
              f"{r['brownouts']:5d} {r['restarts']:8d} {'; '.join(r['fail_steps'])}")
        # Acceptance 5 counts the reset button (and other external resets) pressed while ARMED, not the
        # deliberate panics of the H, T and X keys, which acceptance 4 covers.
        if r["prev_state"] == "ARMED" and r["reset"] not in ("INT_WDT", "TASK_WDT", "PANIC"):
            after_armed.append(r)
    stuck = [r for r in after_armed if not r["reached_disarmed"]]
    print(f"resets while ARMED: {len(after_armed)} | ended DISARMED {len(after_armed) - len(stuck)} | stuck in BOOT"
          f" {len(stuck)} | acceptance 5 (>= 20, each DISARMED or BOOT with a reason): "
          f"{'PASS' if len(after_armed) >= 20 and all(r['reached_disarmed'] or r['fail_steps'] for r in after_armed) else 'FAIL or too few'}")
    browns = max((r["brownouts"] for r in boot_rows), default=0)
    print(f"BROWNOUT resets counted in RTC memory (last boot): {browns}")

    # ---- Task table (acceptance 6).
    if tasks_seen:
        b, tasks, verdict = tasks_seen
        on1 = [t[0] for t in tasks if t[1] in ("1", "any")]
        bad = [t[0] for t in tasks if t[4] != "ok"]
        print(f"task table (boot {b}): {len(tasks)} tasks | can run on Core 1: {', '.join(on1) or 'none'} | not allowed:"
              f" {', '.join(bad) or 'none'} | acceptance 6: {verdict}")
    else:
        print("task table: not found")

    # ---- IMU recovery (acceptance 10).
    if recs:
        armed = [r for r in recs if r[2] == "ARMED"]
        oks = [r for r in recs if r[5] == "OK"]
        # A missing age near 2^32 us is the wrapped negative difference fixed after session 1: a false trigger.
        spurious = [r for r in recs if r[3] > 4.0e6]
        real = [r for r in recs if r[3] <= 4.0e6]
        print(f"IMU recovery: {len(recs)} attempts, {len(oks)} OK, durations ms {stat([r[6] for r in recs])}, attempts"
              f" while ARMED: {len(armed)}, spurious (wrapped age) {len(spurious)}, real {len(real)} | acceptance 10"
              f" (none while ARMED, none spurious, recovery OK): "
              f"{'PASS' if not armed and not spurious and real and all(r[5] == 'OK' for r in real) else 'FAIL'}")
    else:
        print("IMU recovery: no attempts in these logs")

    # ---- Critical section A/B (acceptance 11).
    for k in ("ON", "OFF"):
        c = crit.get(k)
        if c:
            ctl = c["controls"]
            print(f"critical section {k}: controls udp {ctl.get('udp', 0)} ws {ctl.get('ws', 0)} | give-ups {c['giveups']}"
                  f" torn {c['torn']} | seqlock write alone max {c['inside_max']:.2f} us")
    if not crit:
        print("critical section A/B: no 's' summary found")

    # ---- Armed spans (acceptance 14).
    spans = [s for s in soak if s[1] is not None]
    longest = max(spans, key=lambda s: s[1]) if spans else None
    if longest:
        print(f"longest ARMED span: {longest[1]:.1f} s (boot {longest[0]}), ended by: {longest[2]} | acceptance 14"
              f" (>= {SOAK_MIN_S} s, no watchdog cut, no reset): "
              f"{'PASS' if longest[1] >= SOAK_MIN_S and 'wd_' not in longest[2] else 'FAIL or too short'}")
    for s in soak:
        if s[3]:
            print(f"boot {s[0]}: {s[2]}")
    print("===== end =====")
    return 0


if __name__ == "__main__":
    sys.exit(main())
