#!/usr/bin/env python3
"""M7b and M7c log analysis.

M7b: watchdog cuts, hangs and resets, boots after a reset while armed, IMU recovery, the task
table, the critical-section A/B and the armed run (acceptance 14).
M7c: per-condition latency percentiles (acceptance 7 and 8), the R5 table across configurations
(acceptance 9) and the 20-minute soak (acceptance 12), from the "m7c: TM" lines.

Reads one or more PlatformIO monitor logs (joined in the order given; default: the newest
logs/device-monitor-*.log) and uses every M7b boot in them. Standard library only:
    python3 tools/analyse_m7b.py [logfile ...]
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BANNERS = ("M7b bench firmware: no motors may be connected", "M7c bench firmware: no motors may be connected")
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
    starts = [i for i, l in enumerate(lines) if any(b in l for b in BANNERS)]
    if not starts:
        sys.exit("no M7b or M7c boot found")
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
    m7c_report(boots)
    print("===== end =====")
    return 0


# ---------------------------------------------------------------------------------------------
# M7c (docs/M7c_package.md)
JITTER_P999_US = 100       # acceptance 7 and 8: loop-period deviation p99.9
WAKE_MAX_US = 50           # acceptance 7
BURST_TARGET_US = 450 * 1.15   # R5 rule: burst average under page serving within 15 % of 450 us
R5_RUN_S = 300             # addition 4: 300 s per R5 comparison run
SOAK_S = 1200              # acceptance 12
HEAP_DRIFT_B = 2048        # acceptance 12
EXPECTED_SOAK_CUT = "link_silence"

RE_TMLINE = re.compile(
    r"m7c: TM (?P<cond>\w+) (?P<phase>60s|END) t=(?P<secs>[\d.]+) s \| r5 (?P<r5>\S+) load (?P<load>\d+) us(?P<mixed>[^|]*)"
    r"\| iter (?P<iter>\d+) wakes (?P<wakes>\d+) missed (?P<missed>\d+) timeouts (?P<timeouts>\d+) i2c_err (?P<i2c>\d+)"
    r" overruns (?P<overruns>\d+) stale (?P<stale>\d+) \| period n (?P<pn>\d+) dev p50/p99/p99.9/max"
    r" (?P<p50>\d+)/(?P<p99>\d+)/(?P<p999>\d+)/(?P<pmax>\d+) us signed min/max (?P<pmin_s>-?\d+)/(?P<pmax_s>-?\d+)"
    r" over (?P<pover>\d+) \| wake p50/p99/p99.9/max (?P<w50>\d+)/(?P<w99>\d+)/(?P<w999>\d+)/(?P<wmax>\d+) us"
    r" \| burst14 n (?P<b14n>\d+) avg (?P<b14avg>[\d.]+) p50/p99/p99.9/max (?P<b14_50>\d+)/(?P<b14_99>\d+)/(?P<b14_999>\d+)/(?P<b14max>\d+) us"
    r" \| burst6 n (?P<b6n>\d+) avg (?P<b6avg>[\d.]+) p50/p99/p99.9/max (?P<b6_50>\d+)/(?P<b6_99>\d+)/(?P<b6_999>\d+)/(?P<b6max>\d+) us"
    r" \| work avg (?P<wkavg>[\d.]+) p50/p99/p99.9/max (?P<wk50>\d+)/(?P<wk99>\d+)/(?P<wk999>\d+)/(?P<wkmax>\d+) us")
RE_TMSTART = re.compile(r"m7c: TM START (\w+) epoch")
RE_HB = re.compile(r"main: HB t_us=\d+ internal_free=(\d+)")
RE_M7C = re.compile(r"m7c: M7C: latency histograms (\S+) \| R5 config (\S+)")


def tm_parse(l):
    m = RE_TMLINE.search(l)
    if not m:
        return None
    d = m.groupdict()
    out = {k: (v if k in ("cond", "phase", "r5", "mixed") else float(v) if "." in v else int(v)) for k, v in d.items()}
    out["mixed"] = "MIXED" in d["mixed"]
    n = out["b14n"] + out["b6n"]
    out["bavg"] = (out["b14n"] * out["b14avg"] + out["b6n"] * out["b6avg"]) / n if n else 0.0
    out["bmax"] = max(out["b14max"], out["b6max"])
    return out


def m7c_report(boots):
    ends = []        # (boot, record)
    soaks = []       # dicts
    builds = []
    for b, ls in enumerate(boots, 1):
        soak = None
        for l in ls:
            mt = RE_TS.match(l)
            ts = int(mt.group(1)) if mt else None
            if (m := RE_M7C.search(l)):
                builds.append((b, m.group(2), m.group(1)))
            if (m := RE_TMSTART.search(l)):
                if soak and not soak["ended"]:
                    soaks.append(soak)
                soak = None
                if m.group(1) == "soak":
                    soak = dict(boot=b, start_ms=ts, end_ms=None, ended=False, heap=[], cuts={}, arms=0, rec=None,
                                tasks={})
                continue
            r = tm_parse(l)
            if r and r["phase"] == "END":
                ends.append((b, r))
                if soak and r["cond"] == "soak":
                    soak.update(end_ms=ts, ended=True, rec=r)
                    soaks.append(soak)
                    soak = None
                continue
            if soak is None:
                if soaks and soaks[-1]["boot"] == b and soaks[-1]["ended"] and (mm := RE_TASK.search(l)):
                    soaks[-1]["tasks"][mm.group(1)] = int(mm.group(4))  # the table printed after the soak ('s')
                continue
            if (m := RE_HB.search(l)):
                soak["heap"].append(int(m.group(1)))
            elif (m := RE_EV.search(l)):
                text = m.group(3)
                if text.startswith("CUT "):
                    k = text.split()[1]
                    soak["cuts"][k] = soak["cuts"].get(k, 0) + 1
                elif "-> ARMED" in text and "REFUSED" not in text:
                    soak["arms"] += 1
            elif (mm := RE_TASK.search(l)):
                soak["tasks"][mm.group(1)] = int(mm.group(4))
        if soak and not soak["ended"]:
            soaks.append(soak)   # the boot ended inside the soak: a reset (or the end of the log)
    if not builds and not ends:
        return
    print("----- M7c -----")
    for b, r5, hist in builds:
        print(f"boot {b}: R5 config {r5}, latency histograms {hist}")
    if not ends:
        print("M7c: no finished condition (TM ... END) in these logs")
        return

    print(f"{'boot':>4} {'cond':5} {'r5':6} {'load':>4} {'secs':>6} {'missed':>6} {'i2c':>3} {'stale':>5} "
          f"{'per p99.9':>9} {'per max':>7} {'wake max':>8} {'b14 avg/max':>12} {'b6 avg/max':>12} {'b avg':>6} "
          f"{'work p99.9/max':>15}")
    for b, r in ends:
        print(f"{b:4d} {r['cond']:5} {r['r5']:6} {r['load']:4d} {r['secs']:6.0f} {r['missed']:6d} {r['i2c']:3d} "
              f"{r['stale']:5d} {r['p999']:9d} {r['pmax']:7d} {r['wmax']:8d} "
              f"{r['b14avg']:6.1f}/{r['b14max']:<5d} {r['b6avg']:6.1f}/{r['b6max']:<5d} {r['bavg']:6.1f} "
              f"{r['wk999']:7d}/{r['wkmax']:<7d}{' MIXED' if r['mixed'] else ''}")

    def latest(r5, cond, load_on):
        hits = [r for _, r in ends if r["r5"] == r5 and r["cond"] == cond and (r["load"] > 0) == load_on
                and not r["mixed"]]
        return hits[-1] if hits else None

    # ---- Acceptance 7: baseline, load off, per condition.
    ok7 = True
    for cond in ("idle", "udp", "ws", "page"):
        r = latest("a", cond, False)
        if not r:
            print(f"acceptance 7 {cond}: no baseline run (r5 a, load off)")
            ok7 = False
            continue
        good = r["p999"] < JITTER_P999_US and r["wmax"] < WAKE_MAX_US and r["missed"] == 0 and r["i2c"] == 0
        ok7 = ok7 and good
        print(f"acceptance 7 {cond}: period p99.9 {r['p999']} us (< {JITTER_P999_US}), wake max {r['wmax']} us"
              f" (< {WAKE_MAX_US}), missed {r['missed']}, i2c errors {r['i2c']}, burst avg/max {r['b14avg']:.1f}/"
              f"{r['b14max']} us, {r['secs']:.0f} s: {'PASS' if good else 'FAIL'}")
    print(f"acceptance 7: {'PASS' if ok7 else 'FAIL or incomplete'}")

    # ---- Acceptance 8: baseline, page serving, load on.
    r = latest("a", "page", True)
    if r:
        good = r["p999"] < JITTER_P999_US and r["missed"] == 0
        print(f"acceptance 8 (page, load {r['load']} us): period p99.9 {r['p999']} us, missed {r['missed']}, loop work"
              f" p99.9/max {r['wk999']}/{r['wkmax']} us against 1000 us ({1000 - r['wkmax']} us left at the max),"
              f" {r['secs']:.0f} s: {'PASS' if good else 'FAIL'}")
    else:
        print("acceptance 8: no baseline page-serving run with the load on")

    # ---- Acceptance 9: R5 table (load off: page and ws; load on: page).
    configs = []
    for _, r in ends:
        if r["r5"] not in configs:
            configs.append(r["r5"])
    base = latest("a", "page", False)
    print(f"R5 table (rule: burst average under page serving <= {BURST_TARGET_US:.1f} us, with loop work p99.9 and"
          f" max stated; runs of >= {R5_RUN_S} s)")
    print(f"  {'r5':6} {'cond':5} {'load':>4} {'secs':>5} {'burst avg':>9} {'b14 avg/max':>12} {'b6 avg/max':>12}"
          f" {'per p99.9':>9} {'work p99.9/max':>15} {'missed':>6} {'i2c':>3}  note")
    for c in configs:
        for cond, load_on in (("page", False), ("ws", False), ("page", True)):
            r = latest(c, cond, load_on)
            if not r:
                continue
            notes = []
            if r["secs"] < R5_RUN_S - 5:
                notes.append(f"SHORT (< {R5_RUN_S} s)")
            if cond == "page" and not load_on:
                notes.append("meets burst target" if r["bavg"] <= BURST_TARGET_US else "misses burst target")
                if base and c != "a":
                    notes.append(f"work p99.9 {r['wk999'] - base['wk999']:+d} us, max {r['wkmax'] - base['wkmax']:+d} us"
                                 f" vs a")
            print(f"  {c:6} {cond:5} {r['load']:4d} {r['secs']:5.0f} {r['bavg']:9.1f} {r['b14avg']:6.1f}/{r['b14max']:<5d}"
                  f" {r['b6avg']:6.1f}/{r['b6max']:<5d} {r['p999']:9d} {r['wk999']:7d}/{r['wkmax']:<7d} {r['missed']:6d}"
                  f" {r['i2c']:3d}  {'; '.join(notes)}")

    # ---- Acceptance 12: soak.
    for s in soaks:
        r = s["rec"]
        if not s["ended"]:
            print(f"soak (boot {s['boot']}): NOT ENDED: the boot ended inside the soak (a reset, or the log stops)")
            continue
        unintended = {k: v for k, v in s["cuts"].items() if k != EXPECTED_SOAK_CUT}
        heap = s["heap"]
        drift = (heap[-1] - heap[0]) if len(heap) > 1 else 0
        # No leak: the free heap at the end within 2 KB of the start (as M5 acceptance 6). The minimum is
        # reported, not judged: page serving borrows tens of KB per request burst and returns it.
        heap_ok = len(heap) > 1 and abs(drift) <= HEAP_DRIFT_B
        good = (r["secs"] >= SOAK_S - 5 and r["missed"] == 0 and r["i2c"] == 0 and not unintended and heap_ok
                and r["p999"] < JITTER_P999_US)
        print(f"soak (boot {s['boot']}): {r['secs']:.0f} s, load {r['load']} us | missed {r['missed']} i2c errors"
              f" {r['i2c']} | arm cycles {s['arms']}, expected link-loss cuts {s['cuts'].get(EXPECTED_SOAK_CUT, 0)},"
              f" unintended cuts {unintended or 'none'} | period p99.9 {r['p999']} us, work max {r['wkmax']} us"
              f" | internal heap first/last/min {heap[0] if heap else '?'}/{heap[-1] if heap else '?'}/"
              f"{min(heap) if heap else '?'} (drift {drift:+d} B) | acceptance 12: {'PASS' if good else 'FAIL'}")
        if s["tasks"]:
            print("  stack high-water marks (B free, last table in or after the soak): " +
                  ", ".join(f"{k} {v}" for k, v in sorted(s["tasks"].items(), key=lambda kv: kv[1])))
        else:
            print("  stack high-water marks: no task table after the soak (press s at the end)")
    if not soaks:
        print("soak: no 'TM START soak' in these logs")


if __name__ == "__main__":
    sys.exit(main())
