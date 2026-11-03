#!/usr/bin/env python3
"""M6 log analysis: IMU timing in idle windows against UDP and WebSocket traffic windows.

Reads a PlatformIO monitor log (logs/device-monitor-*.log, default: the newest) and pairs each
M3 Phase B window line ("m3: B  wNN ...") with the M6 window line of the same number
("m6: wNN class=... ..."). The firmware labels each 10 s window idle, udp, ws or mixed from the
frames it carried (udp/ws: at least 400 of the 500 expected pings and the other transport
silent). Consecutive ws windows separated by anything else are separate runs (laptop, phone).
Only the last M6 boot in the log is used. Standard library only:
    python3 tools/analyse_m6.py [logfile]

Per-window jitter is the largest deviation of a notify interval from that window's mean
(1e6 / rate); the firmware's whole-run figure uses the whole-run mean, so they differ slightly.
The window lines print the interval sd to 0.1 us.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
M3 = dict(sd=0.44, jitter=3.7, wake_max=12, burst_avg=450.4)
M5 = dict(burst="605-633", wake_max=22, jitter=14.7)
LIMITS = dict(sd=2.0, jitter=20.0, wake_max=50)

RE_M3 = re.compile(r"m3: B\s+w(\d+) [\d.]+s \| rate ([\d.]+) Hz \| iv min/max/sd (\d+)/(\d+)/([\d.]+) us"
                   r" \| burst min/avg/max (\d+)/([\d.]+)/(\d+) us \| wake min/avg/max (\d+)/([\d.]+)/(\d+) us"
                   r" \| missed (\d+) timeouts (\d+) i2c_err (\d+)")
RE_M6 = re.compile(r"m6: w(\d+)(\(partial\))? class=(\w+) (.*)")
RE_KV = re.compile(r"(\w+)=(-?[\d./]+)")


def parse(path):
    text = pathlib.Path(path).read_text(errors="replace").splitlines()
    start = max((i for i, l in enumerate(text) if "m6: M6 bench test:" in l), default=None)
    if start is None:
        sys.exit(f"{path}: no M6 bench test boot found")
    imu, m6 = {}, {}
    for line in text[start:]:
        m = RE_M3.search(line)
        if m:
            g = m.groups()
            w = int(g[0])
            rate, iv_min, iv_max = float(g[1]), int(g[2]), int(g[3])
            mean = 1e6 / rate if rate else 1000.0
            imu[w] = dict(rate=rate, sd=float(g[4]), jitter=max(mean - iv_min, iv_max - mean),
                          burst_avg=float(g[6]), burst_max=int(g[7]), wake_avg=float(g[9]), wake_max=int(g[10]),
                          missed=int(g[11]), timeouts=int(g[12]), i2c=int(g[13]))
            continue
        m = RE_M6.search(line)
        if m:
            w = int(m.group(1))
            kv = {}
            section = ""
            for part in m.group(4).split(" | "):
                head = part.split(" ", 1)[0]
                if head in ("udp", "ws", "sp"):
                    section = head + "_"
                for k, v in RE_KV.findall(part):
                    kv[(section if head in ("udp", "ws", "sp") else "") + k] = v
            m6[w] = dict(cls=m.group(3), partial=bool(m.group(2)), kv=kv)
    return imu, m6


def segments(m6):
    """Labels each window: idle, udp, ws#1, ws#2, ... (a new ws run after any non-ws window)."""
    label, ws_run, prev = {}, 0, None
    for w in sorted(m6):
        c = m6[w]["cls"]
        if c == "ws":
            if prev != "ws":
                ws_run += 1
            label[w] = f"ws#{ws_run}"
        else:
            label[w] = c
        prev = c
    return label


def num(kv, key, default=0.0):
    try:
        return float(kv.get(key, default))
    except ValueError:
        return default


def main():
    if len(sys.argv) > 1:
        path = pathlib.Path(sys.argv[1])
    else:
        logs = sorted((ROOT / "logs").glob("device-monitor-*.log"))
        if not logs:
            sys.exit("no logs/device-monitor-*.log found")
        path = logs[-1]
    imu, m6 = parse(path)
    label = segments(m6)
    groups = {}
    for w, lab in label.items():
        if w in imu and not m6[w]["partial"]:
            groups.setdefault(lab, []).append(w)
    print(f"===== M6 IMU analysis: {path.name} =====")
    print(f"windows: {len(imu)} IMU, {len(m6)} M6; excluded: "
          f"{sum(1 for w in m6 if m6[w]['partial'])} partial, {len(groups.get('mixed', []))} mixed")
    print(f"baselines: M3 sd {M3['sd']} jitter {M3['jitter']} wake max {M3['wake_max']} burst avg {M3['burst_avg']} us;"
          f" M5 under page load: burst avg {M5['burst']}, wake max {M5['wake_max']}, jitter {M5['jitter']} us")
    hdr = (f"{'class':8} {'win':>4} {'sd max':>7} {'jit max':>8} {'wake avg':>9} {'wake max':>9} {'burst avg':>13}"
           f" {'burst max':>10} {'miss/to/i2c':>12} {'sp age max':>11} {'retry max':>10} {'torn':>5} {'stale':>6}"
           f" {'hook avg/max us':>16}  verdict")
    print(hdr)
    order = ["idle", "udp"] + sorted(k for k in groups if k.startswith("ws#")) + ["mixed"]
    all_ok = True
    for lab in order:
        ws = groups.get(lab)
        if not ws:
            continue
        rows = [imu[w] for w in ws]
        kvs = [m6[w]["kv"] for w in ws]
        sd = max(r["sd"] for r in rows)
        jit = max(r["jitter"] for r in rows)
        wake_avg = sum(r["wake_avg"] for r in rows) / len(rows)
        wake_max = max(r["wake_max"] for r in rows)
        b_avgs = [r["burst_avg"] for r in rows]
        burst = f"{min(b_avgs):.0f}-{max(b_avgs):.0f}"
        burst_max = max(r["burst_max"] for r in rows)
        miss = sum(r["missed"] for r in rows), sum(r["timeouts"] for r in rows), sum(r["i2c"] for r in rows)
        age = max(num(k, "sp_age_max") for k in kvs)
        retry = max(num(k, "sp_retry_max") for k in kvs)
        torn = sum(num(k, "sp_torn") for k in kvs)
        stale = sum(num(k, "sp_stale") for k in kvs)
        hooks = [k.get("hook_us", "0/0/0").split("/") for k in kvs]
        h_avg = sum(float(h[1]) for h in hooks) / len(hooks)
        h_max = max(float(h[2]) for h in hooks)
        ok = sd < LIMITS["sd"] and jit < LIMITS["jitter"] and wake_max < LIMITS["wake_max"] and miss == (0, 0, 0)
        if lab != "mixed":
            all_ok = all_ok and ok
        r5 = []
        if wake_max > M3["wake_max"] + 4:
            r5.append("wake")
        if jit > M3["jitter"] + 1:
            r5.append("jitter")
        if max(b_avgs) > M3["burst_avg"] + 1:
            r5.append("burst")
        verdict = ("PASS" if ok else "FAIL") + (f", R5: {' '.join(r5)}" if r5 else "")
        age_s = f"{age / 1000:.1f} ms" if lab != "idle" else "-"
        # Stale reads only mean something while frames are arriving: in idle windows the last setpoint
        # is simply ageing, and mixed windows include the start and end of a run.
        stale_s = f"{stale:.0f}" if lab not in ("idle", "mixed") else "-"
        print(f"{lab:8} {len(ws):4d} {sd:7.2f} {jit:8.1f} {wake_avg:9.1f} {wake_max:9d} {burst:>13} {burst_max:10d}"
              f" {'%d/%d/%d' % miss:>12} {age_s:>11} {retry:10.0f} {torn:5.0f} {stale_s:>6}"
              f" {h_avg:7.2f}/{h_max:<8.2f}  {verdict}")
    print("acceptance 6 (traffic windows: zero missed and I2C errors, sd < 2, jitter < 20, wake max < 50 us): "
          + ("PASS" if all_ok else "FAIL"))
    print("===== end =====")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
