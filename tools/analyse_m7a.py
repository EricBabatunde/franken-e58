#!/usr/bin/env python3
"""M7a log analysis: cut timing, refusals and IMU timing per traffic class.

Reads one or more PlatformIO monitor logs (joined in the order given; default: the newest
logs/device-monitor-*.log) and uses every M7a boot in them, from the first banner on (the bench
procedure includes deliberate resets). Window numbers restart at each boot, so windows are keyed
by (boot, window). Standard library only:
    python3 tools/analyse_m7a.py [logfile ...]

Reports:
  - link-loss cuts (acceptance 5): decision age (150 to 152 ms) and decision -> force-low (< 50 us)
  - kill cuts (acceptance 6): frame receipt -> force-low (< 2 ms)
  - missing data-ready cuts (acceptance 7): last pulse -> decision (> 3000 and <= 4100 us: the check
    runs on the 1 ms tick, so up to one tick plus the wake-up after 3000 us; B-M7a-05)
  - refusal reasons seen (acceptance 3), arms and disarms
  - IMU windows per class (idle, udp, ws, mixed) against the acceptance 9 limits and M6
  - setpoint read give-ups and torn records, and the setpoint write cost (acceptance 11, B-M6-10)
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIMITS = dict(sd=2.0, jitter=20.0, wake_max=50)
M6_BURST = dict(idle="451-459", udp="470-480", ws="514-534")
IMU_CUT_US = (3000, 4100)  # acceptance 7: exclusive lower, inclusive upper bound (B-M7a-05)
TRAFFIC_CTL = 400   # control frames in a 10 s window (50 Hz = 500) to count as that transport's window

RE_B = re.compile(r"m7a: B\s+w(\d+) [\d.]+s \| rate ([\d.]+) Hz \| iv min/max/sd (\d+)/(\d+)/([\d.]+) us"
                  r" \| burst min/avg/max (\d+)/([\d.]+)/(\d+) us \| wake min/avg/max (\d+)/([\d.]+)/(\d+) us"
                  r" \| missed (\d+) timeouts (\d+) i2c_err (\d+)")
RE_LOOP = re.compile(r"m7a: w(\d+) loop \| iterations (\d+) work min/avg/max ([\d.]+)/([\d.]+)/([\d.]+) us overruns (\d+)"
                     r" \| sp reads (\d+) giveups (\d+) retry_max (\d+) torn (\d+) \| armed (\d+) ms"
                     r" \| udp rx=(\d+) ping=(\d+) ctl=(\d+) wr avg/max ([\d.]+)/([\d.]+) us \(inside ([\d.]+)/([\d.]+)\)"
                     r" \| ws rx=(\d+) ping=(\d+) ctl=(\d+) wr avg/max ([\d.]+)/([\d.]+) us \(inside ([\d.]+)/([\d.]+)\)")
RE_LINK = re.compile(r"CUT link_silence \| decision age ([\d.]+) ms .* force-low written ([\d.]+) us after the decision")
RE_KILL = re.compile(r"CUT kill \| frame rx -> decision (-?\d+) us, frame rx -> force-low (-?\d+) us .* force-low ([\d.]+) us")
RE_IMU = re.compile(r"CUT imu_missing \| last data-ready -> decision (-?\d+) us, -> force-low (-?\d+) us .* force-low ([\d.]+) us")
RE_REF = re.compile(r"REFUSED (\w+) \| all reasons: ([\w,]+)")


def load(paths):
    lines = []
    for p in paths:
        lines += pathlib.Path(p).read_text(errors="replace").splitlines()
    starts = [i for i, l in enumerate(lines) if "M7a bench firmware: no motors may be connected" in l]
    if not starts:
        sys.exit("no M7a boot found")
    # The banner is printed three times per boot: a boot starts at the first line of each group.
    boots = [i for i in starts if i - 1 not in starts]
    return [(b, lines[i:j]) for b, (i, j) in enumerate(zip(boots, boots[1:] + [len(lines)]), 1)]


def stat(v):
    return f"min {min(v):.3f} max {max(v):.3f} mean {sum(v) / len(v):.3f}" if v else "none"


def main():
    if len(sys.argv) > 1:
        paths = sys.argv[1:]
    else:
        logs = sorted((ROOT / "logs").glob("device-monitor-*.log"))
        if not logs:
            sys.exit("no logs/device-monitor-*.log found")
        paths = [logs[-1]]
    boots = load(paths)
    imu, loop = {}, {}
    link, kill, imu_cut, refusals = [], [], [], {}
    arms = disarms = 0
    for boot, l in ((b, l) for b, ls in boots for l in ls):
        m = RE_B.search(l)
        if m:
            g = m.groups()
            rate = float(g[1])
            mean = 1e6 / rate if rate else 1000.0
            imu[(boot, int(g[0]))] = dict(sd=float(g[4]), jitter=max(mean - int(g[2]), int(g[3]) - mean), burst_avg=float(g[6]),
                                  burst_max=int(g[7]), wake_max=int(g[10]), missed=int(g[11]), timeouts=int(g[12]),
                                  i2c=int(g[13]))
            continue
        m = RE_LOOP.search(l)
        if m:
            g = m.groups()
            loop[(boot, int(g[0]))] = dict(work_max=float(g[4]), overruns=int(g[5]), giveups=int(g[7]), retry=int(g[8]),
                                   torn=int(g[9]), armed=int(g[10]), udp_ctl=int(g[13]), udp_wr=float(g[14]),
                                   udp_wr_max=float(g[15]), udp_in=float(g[16]), ws_ctl=int(g[20]),
                                   ws_wr=float(g[21]), ws_wr_max=float(g[22]), ws_in=float(g[23]))
            continue
        if "EV #" not in l:
            continue
        if (m := RE_LINK.search(l)):
            link.append((float(m.group(1)), float(m.group(2))))
        elif (m := RE_KILL.search(l)):
            kill.append((int(m.group(1)), int(m.group(2)), float(m.group(3))))
        elif (m := RE_IMU.search(l)):
            imu_cut.append((int(m.group(1)), int(m.group(2)), float(m.group(3))))
        elif (m := RE_REF.search(l)):
            refusals[m.group(1)] = refusals.get(m.group(1), 0) + 1
        elif " ARMED " in l and "-> ARMED" in l:
            arms += 1
        elif "DISARM (arm flag cleared)" in l:
            disarms += 1

    print(f"===== M7a analysis: {', '.join(pathlib.Path(p).name for p in paths)} | M7a boots: {len(boots)} =====")
    ages = [a for a, _ in link]
    forces = [f for _, f in link]
    ok5 = bool(link) and len(link) >= 20 and all(150.0 <= a <= 152.0 for a in ages) and all(f < 50 for f in forces)
    print(f"link-loss cuts: {len(link)} | decision age ms {stat(ages)} | decision -> force-low us {stat(forces)}"
          f" | acceptance 5 timing (>= 20 trials, 150-152 ms, < 50 us): {'PASS' if ok5 else 'FAIL' if link else 'no data'}")
    if kill:
        k = [f for _, f, _ in kill]
        print(f"kill cuts: {len(kill)} | frame rx -> force-low us {stat(k)} | acceptance 6 (< 2000 us): "
              f"{'PASS' if max(k) < 2000 else 'FAIL'}")
    else:
        print("kill cuts: none")
    if imu_cut:
        d = [a for a, _, _ in imu_cut]
        print(f"missing data-ready cuts: {len(imu_cut)} | last pulse -> decision us {stat(d)} | acceptance 7"
              f" (> {IMU_CUT_US[0]} and <= {IMU_CUT_US[1]} us): {'PASS' if all(IMU_CUT_US[0] < x <= IMU_CUT_US[1] for x in d) else 'FAIL'}")
    else:
        print("missing data-ready cuts: none")
    want = ["no_link", "throttle_not_min", "moving", "tilted", "arm_held_no_edge", "imu_unhealthy"]
    seen = ", ".join(f"{k} {v}" for k, v in sorted(refusals.items())) or "none"
    print(f"arms {arms}, disarms {disarms} | refusal events: {seen}")
    print("acceptance 3 refusal reasons seen as events: " +
          ", ".join(f"{r} {'yes' if r in refusals else 'NO'}" for r in want) +
          " (no_link shows in the status line: no frame can carry an arm request without a link)")

    groups = {}
    for w, x in loop.items():
        if w not in imu:
            continue
        # A window with data-ready timeouts is the deliberate 'm' injection (or a real stall): its
        # own row, kept out of the acceptance 9 traffic classes so it can be checked on its own.
        cls = ("m-test" if imu[w]["timeouts"] > 0 else
               "udp" if x["udp_ctl"] >= TRAFFIC_CTL and x["ws_ctl"] == 0 else
               "ws" if x["ws_ctl"] >= TRAFFIC_CTL and x["udp_ctl"] == 0 else
               "idle" if x["udp_ctl"] == 0 and x["ws_ctl"] == 0 else "mixed")
        groups.setdefault(cls, []).append(w)
    print(f"{'class':6} {'win':>4} {'sd max':>7} {'jit max':>8} {'wake max':>9} {'burst avg':>12} {'burst max':>10}"
          f" {'miss/to/i2c':>13}  {'work max us':>12} {'overruns':>9} {'giveups':>8} {'torn':>5}  verdict (M6 burst avg)")
    all_ok = True
    for cls in ("idle", "udp", "ws", "mixed", "m-test"):
        ws = sorted(groups.get(cls, []))
        if not ws:
            continue
        r = [imu[w] for w in ws]
        lp = [loop[w] for w in ws]
        sd = max(x["sd"] for x in r)
        jit = max(x["jitter"] for x in r)
        wake = max(x["wake_max"] for x in r)
        b = [x["burst_avg"] for x in r]
        miss = (sum(x["missed"] for x in r), sum(x["timeouts"] for x in r), sum(x["i2c"] for x in r))
        ok = sd < LIMITS["sd"] and jit < LIMITS["jitter"] and wake < LIMITS["wake_max"] and miss[0] == 0 and miss[2] == 0
        if cls == "m-test":
            ok = miss[0] == 0 and miss[2] == 0  # timeouts and the interval gap are the injection itself
        if cls in ("udp", "ws"):
            all_ok = all_ok and ok
        print(f"{cls:6} {len(ws):4d} {sd:7.2f} {jit:8.1f} {wake:9d} {min(b):5.0f}-{max(b):<6.0f} {max(x['burst_max'] for x in r):10d}"
              f" {'%d/%d/%d' % miss:>13}  {max(x['work_max'] for x in lp):12.1f} {sum(x['overruns'] for x in lp):9d}"
              f" {sum(x['giveups'] for x in lp):8d} {sum(x['torn'] for x in lp):5d}  {'PASS' if ok else 'FAIL'}"
              f" (M6 {M6_BURST.get(cls, '-')}){' (check it matches the m presses)' if cls == 'm-test' else ''}")
    print(f"acceptance 9 (udp and ws windows: zero missed and I2C errors, sd < 2, jitter < 20, wake max < 50 us): "
          f"{'PASS' if all_ok and ('udp' in groups and 'ws' in groups) else 'FAIL or missing traffic class'}"
          " (windows with data-ready timeouts are listed as m-test)")
    for t in ("udp", "ws"):
        lp = [x for x in loop.values() if x[f"{t}_ctl"] > 0]
        if lp:
            n = sum(x[f"{t}_ctl"] for x in lp)
            wr = sum(x[f"{t}_wr"] * x[f"{t}_ctl"] for x in lp) / n
            inner = sum(x[f"{t}_in"] * x[f"{t}_ctl"] for x in lp) / n
            print(f"setpoint writes {t}: {n} | critical section avg {wr:.2f} us (max {max(x[f'{t}_wr_max'] for x in lp):.2f})"
                  f" | seqlock write alone avg {inner:.2f} us | added Core 0 cost avg {wr - inner:.2f} us")
    gu = sum(x["giveups"] for x in loop.values())
    torn = sum(x["torn"] for x in loop.values())
    print(f"Core 1 setpoint reads: give-ups {gu} (expect 0), torn {torn} (must be 0)")
    print("===== end =====")
    return 0


if __name__ == "__main__":
    sys.exit(main())
