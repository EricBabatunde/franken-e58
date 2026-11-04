#!/usr/bin/env python3
"""M7a control client: 14-byte control frames (blueprint 4.6) at 50 Hz over UDP or WebSocket.

Frames are paced on absolute deadlines from time.perf_counter_ns(). The control frame codec and
its CRC come from tools/control_frame.py and are checked against the shared vectors at start.

Interactive keys (single key, no Enter):
    a  arm flag on            d  arm flag off
    t  throttle +50 permille  g  throttle -50 permille
    k  kill flag on/off       s  stop sending (silence)   r  resume sending
    q  quit (sends arm off, throttle 0 for 0.5 s first)

Scripted:  --scenario link_loss_trials N
    each trial: throttle 0 and arm off for 700 ms, arm edge (arm on, throttle 0 for 100 ms),
    throttle 100 permille for 2 s, then silence for 1.5 s; the device should cut at 150 ms of
    silence, latch, and re-arm only on the next trial's fresh edge.

The pong (type 3) has no free field for the device state (B-M6-03), so the device state is read
from the serial log. --ping sends one ping per second and prints its round trip.

Needs the project venv for WebSocket (tools/requirements-m6.txt):
    .venv/bin/python tools/control_client.py --transport udp
    .venv/bin/python tools/control_client.py --transport ws --scenario link_loss_trials 20
"""
import argparse
import asyncio
import contextlib
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import control_frame as cf  # noqa: E402

UDP_PORT = 5858             # board::kUdpPort
RATE_HZ = 50.0
SPIN_NS = 2_000_000         # sleep to within 2 ms of a deadline, then yield-spin
STEP = 50                   # throttle step, permille
FLAG_ARM, FLAG_KILL = 0x01, 0x02


def stamp():
    return time.strftime("%H:%M:%S") + f".{int(time.time() * 1000) % 1000:03d}"


def say(msg):
    print(f"[{stamp()}] {msg}", flush=True)


async def sleep_until(deadline_ns):
    while True:
        left = deadline_ns - time.perf_counter_ns()
        if left <= 0:
            return
        if left > SPIN_NS:
            await asyncio.sleep((left - SPIN_NS) / 1e9)
        else:
            await asyncio.sleep(0)


class Client:
    def __init__(self, args):
        self.args = args
        self.arm = False
        self.kill = False
        self.throttle = 0
        self.sending = True
        self.seq = 0
        self.sent = 0
        self.ping_seq = 0
        self.ping_t = {}
        self.stop = asyncio.Event()
        self._send = None
        self.ws = None

    def state(self):
        return (f"arm {'ON ' if self.arm else 'off'} | kill {'ON ' if self.kill else 'off'} | throttle {self.throttle:4d}"
                f" permille | {'SENDING' if self.sending else 'SILENT '} | frames sent {self.sent}")

    def flags(self):
        return (FLAG_ARM if self.arm else 0) | (FLAG_KILL if self.kill else 0)

    def on_frame(self, data):
        err, f = cf.decode(bytes(data))
        if err != "ok" or f["type"] != cf.PONG:
            say(f"device sent an unexpected frame ({err})")
            return
        t = self.ping_t.pop(f["seq"], None)
        rtt = f"{(time.perf_counter_ns() - t) / 1e6:.2f} ms" if t else "?"
        say(f"pong seq {f['seq']} rtt {rtt}, device turnaround {f['throttle']} us"
            " (device state is not carried by the pong: see the serial log)")

    async def send_loop(self):
        period = int(1e9 / self.args.rate)
        t0 = time.perf_counter_ns() + 100_000_000
        i = 0
        while not self.stop.is_set():
            await sleep_until(t0 + i * period)
            i += 1
            if self.sending and self._send:
                self.seq = (self.seq + 1) & 0xFFFF
                self._send(cf.encode(cf.CONTROL, self.seq, 0, 0, 0, self.throttle, self.flags()))
                self.sent += 1
                if self.args.ping and i % int(self.args.rate) == 0:
                    self.ping_seq = (self.ping_seq + 1) & 0xFFFF
                    self.ping_t[self.ping_seq] = time.perf_counter_ns()
                    self._send(cf.encode(cf.PING, self.ping_seq))

    async def run_udp(self):
        loop = asyncio.get_running_loop()
        client = self

        class Proto(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                client.on_frame(data)

        transport, _ = await loop.create_datagram_endpoint(Proto, remote_addr=(self.args.host, self.args.port))
        self._send = transport.sendto
        try:
            await self.stop.wait()
        finally:
            transport.close()

    async def run_ws(self):
        from websockets.asyncio.client import connect
        from websockets.exceptions import ConnectionClosed

        uri = f"ws://{self.args.host}/ws"
        while not self.stop.is_set():
            try:
                async with connect(uri, compression=None, max_size=64, ping_interval=None, open_timeout=5) as ws:
                    say(f"WebSocket connected to {uri} (a reconnect never arms: the device needs a fresh edge)")
                    self._send = lambda frame: asyncio.ensure_future(self._ws_send(ws, frame))
                    reader = asyncio.create_task(self._ws_reader(ws))
                    stopper = asyncio.create_task(self.stop.wait())
                    done, _ = await asyncio.wait({reader, stopper}, return_when=asyncio.FIRST_COMPLETED)
                    if stopper in done:
                        self._send = None
                        reader.cancel()
                        await ws.close()  # normal close (1000)
                        return
                    stopper.cancel()
                    reader.result()  # raises ConnectionClosed if the device dropped the link
            except (OSError, ConnectionClosed, ConnectionError, asyncio.TimeoutError) as e:
                self._send = None
                if self.stop.is_set():
                    break
                say(f"WebSocket lost ({e!r}); reconnecting in 1 s")
                await asyncio.sleep(1.0)

    async def _ws_reader(self, ws):
        async for msg in ws:
            if isinstance(msg, (bytes, bytearray)):
                self.on_frame(msg)
        raise ConnectionError("closed by the device")

    async def _ws_send(self, ws, frame):
        with contextlib.suppress(Exception):  # a dropped link is reported by run_ws
            await ws.send(frame)

    async def quit(self):
        say("quitting: arm off, throttle 0 for 0.5 s")
        self.arm = False
        self.kill = False
        self.throttle = 0
        self.sending = True
        await asyncio.sleep(0.5)
        self.stop.set()

    def key(self, ch):
        if ch == "a":
            self.arm = True
        elif ch == "d":
            self.arm = False
        elif ch == "t":
            self.throttle = min(1000, self.throttle + STEP)
        elif ch == "g":
            self.throttle = max(0, self.throttle - STEP)
        elif ch == "k":
            self.kill = not self.kill
        elif ch == "s":
            self.sending = False
        elif ch == "r":
            self.sending = True
        elif ch == "q":
            asyncio.ensure_future(self.quit())
            return
        else:
            say("keys: a arm on, d arm off, t/g throttle +/-50, k kill, s silence, r resume, q quit")
            return
        say(f"key {ch}: {self.state()}")

    async def link_loss_trials(self, n):
        await asyncio.sleep(0.3)
        for trial in range(1, n + 1):
            self.sending, self.kill, self.arm, self.throttle = True, False, False, 0
            say(f"trial {trial}/{n}: step 1 throttle 0, arm off, sending (700 ms)")
            await asyncio.sleep(0.7)
            self.arm = True
            say(f"trial {trial}/{n}: step 2 arm edge: arm on, throttle 0 (100 ms) -> device should ARM")
            await asyncio.sleep(0.1)
            self.throttle = 100
            say(f"trial {trial}/{n}: step 3 throttle 100 permille (2 s) -> outputs at "
                f"{100 * self.args.cap // 1000} permille duty")
            await asyncio.sleep(2.0)
            self.sending = False
            say(f"trial {trial}/{n}: step 4 SILENCE (1.5 s) -> device should CUT at 150 ms, latched")
            await asyncio.sleep(1.5)
        self.sending, self.arm, self.throttle = True, False, 0
        say(f"{n} trials done; sending arm off, throttle 0")
        await asyncio.sleep(0.5)
        self.stop.set()


async def main_async(args):
    c = Client(args)
    say(f"control client: {args.transport} to {args.host}"
        f"{':' + str(args.port) if args.transport == 'udp' else '/ws'}, {args.rate:.0f} Hz")
    say(c.state())
    tasks = [asyncio.create_task(c.send_loop()),
             asyncio.create_task(c.run_udp() if args.transport == "udp" else c.run_ws())]
    loop = asyncio.get_running_loop()
    restore = None
    if args.scenario:
        tasks.append(asyncio.create_task(c.link_loss_trials(args.scenario_n)))
    elif sys.stdin.isatty():
        import termios
        import tty
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        tty.setcbreak(fd)
        restore = lambda: termios.tcsetattr(fd, termios.TCSADRAIN, old)  # noqa: E731
        loop.add_reader(fd, lambda: c.key(sys.stdin.read(1)))
        say("keys: a arm on, d arm off, t/g throttle +/-50, k kill, s silence, r resume, q quit")
    else:
        say("stdin is not a terminal: no keys; Ctrl-C to stop")
    try:
        await c.stop.wait()
    finally:
        if restore:
            loop.remove_reader(sys.stdin.fileno())
            restore()
        await asyncio.wait(tasks, timeout=1.0)  # let the WebSocket close normally
        for t in tasks:
            t.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
    say(f"stopped after {c.sent} control frames")
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--transport", choices=("udp", "ws"), default="udp")
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--port", type=int, default=UDP_PORT, help="UDP port (default 5858)")
    p.add_argument("--rate", type=float, default=RATE_HZ, help="control frames per second (default 50)")
    p.add_argument("--scenario", nargs=2, metavar=("NAME", "N"), help="link_loss_trials N")
    p.add_argument("--cap", type=int, default=200, help="device bench duty cap, for the printout only (default 200)")
    p.add_argument("--ping", action="store_true", help="one ping per second, round trip printed")
    args = p.parse_args()
    if args.scenario:
        if args.scenario[0] != "link_loss_trials":
            p.error("the only scenario is link_loss_trials N")
        args.scenario_n = int(args.scenario[1])
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
