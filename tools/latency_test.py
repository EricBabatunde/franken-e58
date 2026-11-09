#!/usr/bin/env python3
"""M6 latency harness (blueprint section 4.6, target: one-way p95 < 15 ms, p99 < 20 ms).

Sends 14-byte ping frames at --rate (default 50 Hz) over UDP or the /ws WebSocket, plus a
control frame half a period after each ping (D-M6-04; --no-control turns them off), and
matches the pongs by seq. One-way latency is estimated as RTT/2. The pong's throttle field
carries the device turnaround (receive to send, us), so the network part is RTT minus that.

Sends are paced on absolute deadlines from time.perf_counter_ns(); the lateness of every send
against its deadline is recorded as the sender's own scheduling jitter.

If the WebSocket drops (device reset, brownout, out of range), sending stops, the run is
summarised over the pings actually sent, and RESULT is FAIL with the reason.

Loss above 0.5 % (LOSS_FLAG_PCT, lost or later than 200 ms) is also a FAIL (M7c close-out): the
percentiles describe only the pongs that came back, so a run that lost most of its pings could
otherwise print PASS (M7c soak, 99.8 % lost).

Outputs: <out>.csv (every ping), <out>.json (summary), <out>.png (histogram, 0.5 ms bins).

Needs the project venv (tools/requirements-m6.txt):
    .venv/bin/python tools/latency_test.py --transport udp
    .venv/bin/python tools/latency_test.py --transport ws
    .venv/bin/python tools/latency_test.py --selftest      # local fake device, no hardware
"""
import argparse
import asyncio
import csv
import datetime
import json
import pathlib
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import control_frame as cf  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
UDP_PORT = 5858             # board::kUdpPort
TIMEOUT_S = 0.200           # a pong later than this counts as lost
TARGET_P95_MS = 15.0
TARGET_P99_MS = 20.0
LOSS_FLAG_PCT = 0.5
SPIN_NS = 3_000_000         # sleep to within 3 ms of a deadline, then yield-spin (asyncio overshoots ~1.5 ms)
PING_MARK = (0x1234, -0x1234, 0x0E58, cf.FLAG_ANGLE)  # roll, pitch, yaw, flags in every ping


class Run:
    """Bookkeeping for one run: sent pings, matched pongs, errors."""

    def __init__(self, count):
        self.count = count
        self.t_send = [0] * count
        self.deadline = [0] * count
        self.t_recv = [None] * count
        self.turnaround = [None] * count
        self.outstanding = {}       # seq -> index
        self.max_index_rx = -1
        self.out_of_order = 0
        self.late = 0               # pongs that arrived after the timeout
        self.unknown = 0            # pongs with no matching ping (duplicate or unknown seq)
        self.bad_rx = 0             # received frames that failed validation or were not pongs
        self.echo_errors = 0        # pong fields not echoed
        self.controls_sent = 0
        self.control_lateness = []
        self.n_sent = 0             # pings sent; less than count if the link was lost
        self.link_lost = None       # reason, if the connection dropped mid-run

    def sent(self, i, deadline_ns, t_ns):
        self.deadline[i] = deadline_ns
        self.t_send[i] = t_ns
        self.outstanding[i & 0xFFFF] = i

    def on_frame(self, buf, t_ns):
        err, f = cf.decode(bytes(buf))
        if err != "ok" or f["type"] != cf.PONG:
            self.bad_rx += 1
            return
        i = self.outstanding.pop(f["seq"], None)
        if i is None:
            self.unknown += 1
            return
        if (f["roll"], f["pitch"], f["yaw"], f["flags"]) != PING_MARK:
            self.echo_errors += 1
        if t_ns - self.t_send[i] > TIMEOUT_S * 1e9:
            self.late += 1
            return
        self.t_recv[i] = t_ns
        self.turnaround[i] = f["throttle"]
        if i < self.max_index_rx:
            self.out_of_order += 1
        self.max_index_rx = max(self.max_index_rx, i)


async def sleep_until(deadline_ns):
    while True:
        left = deadline_ns - time.perf_counter_ns()
        if left <= 0:
            return
        if left > SPIN_NS:
            await asyncio.sleep((left - SPIN_NS) / 1e9)
        else:
            await asyncio.sleep(0)


async def send_loop(run, send, rate, control):
    period = int(1e9 / rate)
    t0 = time.perf_counter_ns() + 200_000_000
    roll, pitch, yaw, flags = PING_MARK
    for i in range(run.count):
        deadline = t0 + i * period
        await sleep_until(deadline)
        if run.link_lost:
            return
        frame = cf.encode(cf.PING, i, roll, pitch, yaw, 0, flags)
        t = time.perf_counter_ns()
        send(frame)
        run.sent(i, deadline, t)
        run.n_sent = i + 1
        if control:
            d2 = deadline + period // 2
            await sleep_until(d2)
            # Neutral sticks, throttle 0, arm bit clear: nothing acts on the setpoint in M6.
            send(cf.encode(cf.CONTROL, i, 0, 0, 0, 0, 0))
            run.control_lateness.append(time.perf_counter_ns() - d2)
            run.controls_sent += 1
    await asyncio.sleep(TIMEOUT_S + 0.1)


class UdpClient(asyncio.DatagramProtocol):
    def __init__(self, run):
        self.run = run

    def datagram_received(self, data, addr):
        self.run.on_frame(data, time.perf_counter_ns())


async def run_udp(host, port, run, rate, control):
    loop = asyncio.get_running_loop()
    transport, _ = await loop.create_datagram_endpoint(lambda: UdpClient(run), remote_addr=(host, port))
    try:
        await send_loop(run, transport.sendto, rate, control)
    finally:
        transport.close()


async def run_ws(uri, run, rate, control):
    from websockets.asyncio.client import connect
    from websockets.exceptions import ConnectionClosed

    def lost(why):
        if run.link_lost is None:
            run.link_lost = f"WebSocket lost after {run.n_sent} of {run.count} pings: {why}"
            print(f"LINK LOST: {run.link_lost}", file=sys.stderr)

    async with connect(uri, compression=None, max_size=64, ping_interval=None, open_timeout=10) as ws:
        async def receiver():
            try:
                async for msg in ws:
                    run.on_frame(msg if isinstance(msg, (bytes, bytearray)) else b"", time.perf_counter_ns())
            except ConnectionClosed as e:
                lost(repr(e.__cause__ or e))
            else:
                lost(f"closed by the device ({ws.close_code} {ws.close_reason!r})")

        rx = asyncio.create_task(receiver())
        pending = set()

        def sent(t):
            pending.discard(t)
            if not t.cancelled() and t.exception() is not None:  # retrieving it also silences asyncio
                lost(repr(t.exception().__cause__ or t.exception()))

        def send(frame):
            if run.link_lost:
                return
            t = asyncio.ensure_future(ws.send(frame))
            pending.add(t)
            t.add_done_callback(sent)

        try:
            await send_loop(run, send, rate, control)
        finally:
            rx.cancel()


def pct(a, p):
    return float(np.percentile(a, p)) if len(a) else float("nan")


def stats(a):
    a = np.asarray(a, dtype=float)
    if not len(a):
        return None
    return dict(n=int(len(a)), min=float(a.min()), mean=float(a.mean()), p50=pct(a, 50), p95=pct(a, 95),
                p99=pct(a, 99), p99_9=pct(a, 99.9), max=float(a.max()))


def summarise(run, args, transport, target):
    n = run.n_sent
    ok = [i for i in range(n) if run.t_recv[i] is not None]
    rtt_us = np.array([(run.t_recv[i] - run.t_send[i]) / 1000 for i in ok])
    ta_us = np.array([run.turnaround[i] for i in ok], dtype=float)
    lateness_us = np.array([(run.t_send[i] - run.deadline[i]) / 1000 for i in range(n)])
    lost = n - len(ok)
    one_way_ms = rtt_us / 2000
    s = dict(
        transport=transport, target=target, rate_hz=args.rate, count=n, planned=run.count, link_lost=run.link_lost,
        control_frames=run.controls_sent, received=len(ok), lost=lost, loss_pct=100.0 * lost / n if n else 0.0,
        late_after_timeout=run.late, out_of_order=run.out_of_order, unknown_pongs=run.unknown,
        invalid_rx=run.bad_rx, echo_errors=run.echo_errors,
        one_way_ms=stats(one_way_ms), rtt_ms=stats(rtt_us / 1000), device_turnaround_us=stats(ta_us),
        network_ms=stats((rtt_us - ta_us) / 1000),
        sender_lateness_us=dict(ping=stats(lateness_us),
                                control=stats(np.array(run.control_lateness) / 1000)),
        timestamp=datetime.datetime.now().isoformat(timespec="seconds"),
    )
    ow = s["one_way_ms"]
    s["pass_p95"] = bool(ow and ow["p95"] < TARGET_P95_MS)
    s["pass_p99"] = bool(ow and ow["p99"] < TARGET_P99_MS)
    s["loss_flag"] = s["loss_pct"] > LOSS_FLAG_PCT
    reasons = []
    if not s["pass_p95"]:
        reasons.append("p95")
    if not s["pass_p99"]:
        reasons.append("p99")
    if s["loss_flag"]:
        reasons.append(f"loss {s['loss_pct']:.2f} % > {LOSS_FLAG_PCT} %")
    if run.bad_rx:
        reasons.append("invalid frames")
    if run.echo_errors:
        reasons.append("echo errors")
    if run.link_lost:
        reasons.append("link lost")
    s["fail_reasons"] = reasons
    s["result"] = "FAIL" if reasons else "PASS"
    return s, one_way_ms


def write_outputs(run, s, one_way_ms, prefix):
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with open(f"{prefix}.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["index", "seq", "deadline_ns", "t_send_ns", "t_recv_ns", "rtt_us", "one_way_ms",
                    "turnaround_us", "network_ms", "send_lateness_us", "status"])
        for i in range(run.n_sent):
            r = run.t_recv[i]
            rtt = (r - run.t_send[i]) / 1000 if r is not None else ""
            ta = run.turnaround[i] if r is not None else ""
            w.writerow([i, i & 0xFFFF, run.deadline[i], run.t_send[i], r if r is not None else "",
                        f"{rtt:.1f}" if r is not None else "", f"{rtt / 2000:.4f}" if r is not None else "",
                        ta, f"{(rtt - ta) / 1000:.4f}" if r is not None else "",
                        f"{(run.t_send[i] - run.deadline[i]) / 1000:.1f}", "ok" if r is not None else "lost"])
    pathlib.Path(f"{prefix}.json").write_text(json.dumps(s, indent=2))
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(9, 4.5))
    if len(one_way_ms):
        top = max(float(one_way_ms.max()), TARGET_P99_MS) + 0.5
        ax.hist(one_way_ms, bins=np.arange(0, top + 0.5, 0.5), color="#3060a0", log=True)
    ow = s["one_way_ms"] or {}
    for v, name, col in ((TARGET_P95_MS, "p95 target", "#d08000"), (TARGET_P99_MS, "p99 target", "#c03030")):
        ax.axvline(v, color=col, linestyle="--", linewidth=1, label=name)
    for key, col in (("p95", "#d08000"), ("p99", "#c03030")):
        if key in ow:
            ax.axvline(ow[key], color=col, linewidth=1.5, label=f"measured {key} {ow[key]:.2f} ms")
    ax.set_xlabel("one-way latency estimate, RTT/2 (ms), 0.5 ms bins")
    ax.set_ylabel("pings (log)")
    ax.set_title(f"M6 {s['transport']} to {s['target']}: {s['received']}/{s['count']} pongs, "
                 f"loss {s['loss_pct']:.2f} %")
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(f"{prefix}.png", dpi=120)
    plt.close(fig)


def fmt(d, unit, scale=1.0, digits=2):
    if not d:
        return "n/a"
    k = ("min", "mean", "p50", "p95", "p99", "p99_9", "max")
    return "/".join(f"{d[x] * scale:.{digits}f}" for x in k) + f" {unit}"


def print_summary(s, prefix):
    print("===== M6 latency summary =====")
    print(f"{s['transport']} -> {s['target']}, {s['rate_hz']:.0f} Hz, {s['count']} pings, "
          f"{s['control_frames']} control frames")
    print(f"received {s['received']}, lost {s['lost']} ({s['loss_pct']:.2f} %{' FLAG > 0.5 %' if s['loss_flag'] else ''}),"
          f" of which late (> {TIMEOUT_S * 1000:.0f} ms) {s['late_after_timeout']}; out of order {s['out_of_order']};"
          f" unknown {s['unknown_pongs']}; invalid {s['invalid_rx']}; echo errors {s['echo_errors']}")
    print("                     min/mean/p50/p95/p99/p99.9/max")
    print(f"one-way (RTT/2)    : {fmt(s['one_way_ms'], 'ms')}")
    print(f"RTT                : {fmt(s['rtt_ms'], 'ms')}")
    print(f"device turnaround  : {fmt(s['device_turnaround_us'], 'us', digits=0)}")
    print(f"network (RTT - ta) : {fmt(s['network_ms'], 'ms')}")
    print(f"sender lateness    : ping {fmt(s['sender_lateness_us']['ping'], 'us', digits=0)}")
    if s["sender_lateness_us"]["control"]:
        print(f"                     control {fmt(s['sender_lateness_us']['control'], 'us', digits=0)}")
    ow = s["one_way_ms"] or {}
    print(f"targets: p95 {ow.get('p95', float('nan')):.2f} < {TARGET_P95_MS:.0f} ms {'PASS' if s['pass_p95'] else 'FAIL'},"
          f" p99 {ow.get('p99', float('nan')):.2f} < {TARGET_P99_MS:.0f} ms {'PASS' if s['pass_p99'] else 'FAIL'}")
    if s["link_lost"]:
        print(f"LINK LOST: {s['link_lost']} (planned {s['planned']}; figures cover the pings sent)")
    if prefix:
        print(f"files: {prefix}.csv, .json, .png")
    print(f"RESULT: {s['result']}" + (f" ({', '.join(s['fail_reasons'])})" if s["fail_reasons"] else ""))
    print("===== end =====")


async def measure(transport, host, args, port=UDP_PORT, ws_uri=None):
    run = Run(args.count)
    if transport == "udp":
        await run_udp(host, port, run, args.rate, not args.no_control)
        target = f"{host}:{port}"
    else:
        uri = ws_uri or f"ws://{host}/ws"
        await run_ws(uri, run, args.rate, not args.no_control)
        target = uri
    return run, target


# ---- Self-test: a local fake device with the same validation and pong rules as the firmware.

class FakeDevice:
    def __init__(self):
        self.rx = self.controls = self.pings = self.invalid = self.ws_closed_bad = 0

    def handle(self, buf):
        """Returns the pong bytes, None for a control frame, or False for an invalid frame."""
        t = time.perf_counter_ns()
        self.rx += 1
        err, f = cf.decode(bytes(buf))
        if err != "ok" or f["type"] == cf.PONG:
            self.invalid += 1
            return False
        if f["type"] == cf.CONTROL:
            self.controls += 1
            return None
        self.pings += 1
        return cf.make_pong(f, (time.perf_counter_ns() - t) // 1000)


class FakeUdp(asyncio.DatagramProtocol):
    def __init__(self, dev):
        self.dev = dev

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data, addr):
        out = self.dev.handle(data)
        if out:
            self.transport.sendto(out, addr)


async def selftest(args):
    from websockets.asyncio.client import connect
    from websockets.asyncio.server import serve
    from websockets.exceptions import ConnectionClosed

    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))
        print(f"  {'PASS' if ok else 'FAIL'}: {what}")

    print("===== M6 latency tool self-test (local fake device, no hardware) =====")
    n, failures = cf.run_vectors()
    check(not failures and n >= 30, f"control frame vectors: {n} checks, {len(failures)} failed")

    dev = FakeDevice()
    loop = asyncio.get_running_loop()
    udp_t, _ = await loop.create_datagram_endpoint(lambda: FakeUdp(dev), local_addr=("127.0.0.1", 0))
    udp_port = udp_t.get_extra_info("sockname")[1]

    async def ws_handler(ws):
        if ws.request.path not in ("/ws", "/drop"):
            await ws.close(1008)
            return
        n_rx = 0
        async for msg in ws:
            n_rx += 1
            if ws.request.path == "/drop" and n_rx == 100:
                ws.transport.abort()  # like a device reset: no close frame
                return
            out = dev.handle(msg) if isinstance(msg, bytes) else False
            if out is False:
                dev.ws_closed_bad += 1
                await ws.close(1003)
                return
            if out:
                await ws.send(out)

    server = await serve(ws_handler, "127.0.0.1", 0, compression=None)
    ws_port = server.sockets[0].getsockname()[1]
    ws_uri = f"ws://127.0.0.1:{ws_port}/ws"
    results = {}
    try:
        for transport in ("udp", "ws"):
            print(f"-- {transport}: {args.count} pings at {args.rate:.0f} Hz against the fake device")
            run, target = await measure(transport, "127.0.0.1", args, port=udp_port, ws_uri=ws_uri)
            s, one_way = summarise(run, args, transport, target)
            prefix = out_prefix(args, f"selftest-{transport}")
            write_outputs(run, s, one_way, prefix)
            print_summary(s, prefix)
            results[transport] = s
            check(s["received"] == run.count and s["lost"] == 0, f"{transport}: every ping answered")
            check(s["invalid_rx"] == 0 and s["echo_errors"] == 0, f"{transport}: pongs valid and echoed")
            check(s["out_of_order"] == 0, f"{transport}: no out-of-order pongs on loopback")
            check(all(pathlib.Path(f"{prefix}.{e}").stat().st_size > 0 for e in ("csv", "json", "png")),
                  f"{transport}: CSV, JSON and PNG written")
        expected_controls = 0 if args.no_control else 2 * args.count
        check(dev.controls == expected_controls, f"fake device saw {dev.controls} control frames"
                                                 f" (expected {expected_controls})")
        # Invalid frames: UDP drops silently, WebSocket closes the session.
        before = dev.invalid
        sock_t, _ = await loop.create_datagram_endpoint(asyncio.DatagramProtocol, remote_addr=("127.0.0.1", udp_port))
        good = cf.encode(cf.PING, 1)
        sock_t.sendto(good[:13] + bytes([good[13] ^ 1]))
        await asyncio.sleep(0.1)
        sock_t.close()
        check(dev.invalid == before + 1, "UDP: bad-CRC frame counted and dropped")
        async with connect(ws_uri, compression=None) as ws:
            await ws.send(b"\xE5" * 15)
            try:
                await asyncio.wait_for(ws.recv(), 2)
                closed = False
            except ConnectionClosed:
                closed = True
        check(closed and dev.ws_closed_bad == 1, "WebSocket: wrong-length frame closes the session")
        # Link loss mid-run: the tool stops sending, reports it, and leaves no unretrieved task errors.
        errors = []
        loop.set_exception_handler(lambda _loop, ctx: errors.append(ctx.get("message")))
        drop_args = argparse.Namespace(**{**vars(args), "count": 200})
        run, target = await measure("ws", "127.0.0.1", drop_args, ws_uri=f"ws://127.0.0.1:{ws_port}/drop")
        await asyncio.sleep(0.1)
        import gc
        gc.collect()
        await asyncio.sleep(0)
        loop.set_exception_handler(None)
        s, _ = summarise(run, drop_args, "ws", target)
        check(run.link_lost and run.n_sent < 60 and s["result"] == "FAIL" and not errors,
              f"WebSocket: dropped link stops the run ({run.n_sent} of 200 sent, {len(errors)} stray task errors)")
        # Heavy loss with good percentiles (M7c soak: 99.8 % lost, the 0.2 % answered were fast): FAIL.
        lossy = Run(100)
        lossy.n_sent = 100
        for i in range(100):
            lossy.sent(i, i * 20_000_000, i * 20_000_000)
            if i % 10 < 4:  # 40 % answered in 2 ms, 60 % lost
                lossy.t_recv[i] = lossy.t_send[i] + 2_000_000
                lossy.turnaround[i] = 100
        s, _ = summarise(lossy, drop_args, "ws", "synthetic")
        check(s["loss_flag"] and s["pass_p95"] and s["pass_p99"] and s["result"] == "FAIL"
              and any("loss" in r for r in s["fail_reasons"]), "loss > 0.5 % with good percentiles: FAIL")
    finally:
        udp_t.close()
        server.close()
        await server.wait_closed()

    floor = {t: (results[t]["rtt_ms"]["min"], results[t]["rtt_ms"]["p50"], results[t]["rtt_ms"]["p99"])
             for t in results}
    print("host RTT floor (loopback, tool + fake device), min/p50/p99 ms: " +
          ", ".join(f"{t} {a:.3f}/{b:.3f}/{c:.3f}" for t, (a, b, c) in floor.items()))
    for t in results:
        lat = results[t]["sender_lateness_us"]["ping"]
        print(f"sender scheduling lateness ({t}) min/mean/p99/max: "
              f"{lat['min']:.0f}/{lat['mean']:.0f}/{lat['p99']:.0f}/{lat['max']:.0f} us")
    ok = all(c for c, _ in checks)
    print(f"SELFTEST: {'PASS' if ok else 'FAIL'} ({sum(c for c, _ in checks)}/{len(checks)} checks)")
    return 0 if ok else 1


def out_prefix(args, name):
    if args.out:
        return pathlib.Path(args.out if name.startswith(("udp", "ws")) else f"{args.out}-{name}")
    stamp = datetime.datetime.now().strftime("%y%m%d-%H%M%S")
    return ROOT / "logs" / "m6" / f"{name}-{stamp}"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--transport", choices=("udp", "ws"), default="udp")
    p.add_argument("--rate", type=float, default=50.0, help="pings per second (default 50)")
    p.add_argument("--count", type=int, default=None, help="pings to send (default 10000; self-test 500)")
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--port", type=int, default=UDP_PORT, help="UDP port (default 5858)")
    p.add_argument("--out", default=None, help="output prefix (default logs/m6/<transport>-<time>)")
    p.add_argument("--no-control", action="store_true", help="pings only, no control frames")
    p.add_argument("--selftest", action="store_true", help="run against a local fake device")
    args = p.parse_args()
    if args.count is None:
        args.count = 500 if args.selftest else 10000
    if args.selftest:
        return asyncio.run(selftest(args))
    run, target = asyncio.run(measure(args.transport, args.host, args, port=args.port))
    s, one_way = summarise(run, args, args.transport, target)
    prefix = out_prefix(args, args.transport)
    write_outputs(run, s, one_way, prefix)
    print_summary(s, prefix)
    return 0 if s["result"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
