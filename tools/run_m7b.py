#!/usr/bin/env python3
"""M7c soak driver: repeated arm cycles over UDP or WebSocket, reusing tools/control_client.py.

Each cycle (control frames at 50 Hz throughout, except during the silence):
    1. throttle 0, arm off, kill off, sending        (0.7 s: throttle-minimum hold, arm seen low)
    2. arm on, throttle 0                             (0.1 s: the fresh arm edge -> device ARMED)
    3. throttle 100 permille                          (20 s by default)
    4. silence: no frames                             (1.0 s: link-loss cut at 150 ms, latched)
It sends nothing else: no kill, no pings, no fault of any kind. The cut in step 4 is the expected
one; any other cut, and any reset, is unintended (tools/analyse_m7b.py counts them).

Soak (docs/M7c_package.md), three terminals plus the monitor, synthetic load on (key L), key 4:
    .venv/bin/python tools/run_m7b.py --transport udp --minutes 20
    .venv/bin/python tools/latency_test.py --transport ws --no-control --count 60000
    .venv/bin/python tools/m5_http_test.py --load 1200
"""
import argparse
import asyncio
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import control_client as ccl  # noqa: E402
import control_frame as cf  # noqa: E402


async def arm_cycles(c, minutes, hold_s, silence_s):
    await asyncio.sleep(0.3)
    end = time.monotonic() + minutes * 60.0
    n = 0
    while time.monotonic() < end and not c.stop.is_set():
        n += 1
        c.sending, c.kill, c.arm, c.throttle = True, False, False, 0
        ccl.say(f"cycle {n}: throttle 0, arm off (0.7 s)")
        await asyncio.sleep(0.7)
        c.arm = True
        ccl.say(f"cycle {n}: arm edge (0.1 s) -> device should ARM")
        await asyncio.sleep(0.1)
        c.throttle = 100
        ccl.say(f"cycle {n}: throttle 100 permille ({hold_s:.0f} s)")
        await asyncio.sleep(hold_s)
        c.sending = False
        ccl.say(f"cycle {n}: SILENCE ({silence_s:.1f} s) -> expected link-loss cut at 150 ms")
        await asyncio.sleep(silence_s)
    c.sending, c.arm, c.throttle = True, False, 0
    ccl.say(f"{n} cycles done in {minutes:.1f} min; sending arm off, throttle 0")
    await asyncio.sleep(0.5)
    c.stop.set()
    return n


async def main_async(args):
    c = ccl.Client(args)
    ccl.say(f"run_m7b: {args.transport} to {args.host}"
            f"{':' + str(args.port) if args.transport == 'udp' else '/ws'}, {args.rate:.0f} Hz, "
            f"{args.minutes:.1f} min, throttle 100 for {args.hold:.0f} s per cycle")
    tasks = [asyncio.create_task(c.send_loop()),
             asyncio.create_task(c.run_udp() if args.transport == "udp" else c.run_ws())]
    cycles = asyncio.create_task(arm_cycles(c, args.minutes, args.hold, args.silence))
    try:
        await c.stop.wait()
    finally:
        await asyncio.wait(tasks, timeout=1.0)
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
    n = cycles.result() if cycles.done() and not cycles.cancelled() else 0
    ccl.say(f"RUN_M7B: {n} arm cycles, {c.sent} control frames sent")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--transport", choices=("udp", "ws"), default="udp")
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--port", type=int, default=ccl.UDP_PORT, help="UDP port (default 5858)")
    p.add_argument("--rate", type=float, default=ccl.RATE_HZ, help="control frames per second (default 50)")
    p.add_argument("--minutes", type=float, default=20.0, help="run length (default 20)")
    p.add_argument("--hold", type=float, default=20.0, help="seconds at throttle 100 per cycle (default 20)")
    p.add_argument("--silence", type=float, default=1.0, help="silence per cycle, s (default 1.0; cut at 0.15)")
    p.add_argument("--cap", type=int, default=200, help="device bench duty cap, for Client printouts only")
    args = p.parse_args()
    args.ping = False  # Client option: never ping here (pings come from latency_test.py)
    n, failures = cf.run_vectors()
    if failures:
        print(f"control frame vectors: {len(failures)} of {n} checks FAILED; not sending", file=sys.stderr)
        return 1
    print(f"control frame vectors: {n} checks, 0 failed")
    try:
        return asyncio.run(main_async(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
