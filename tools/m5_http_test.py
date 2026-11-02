#!/usr/bin/env python3
"""M5 HTTP client (Python standard library only). Run on a laptop joined to the AP.

Default mode:
  - GET / 200 times: success count and min/avg/max latency.
  - GET /test.bin 20 times: length and SHA-256 after gzip decoding, and throughput.
  - Negative cases: unknown path (404), POST / (405), paths with ".." (400), and /version.
--load SECONDS:
  - GET / every 50 ms and /test.bin every 2 s for SECONDS (for M5-IMU-2).
Prints one summary block.
"""
import argparse
import gzip
import hashlib
import http.client
import importlib.util
import pathlib
import statistics
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("build_web", HERE / "build_web.py")
build_web = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(build_web)

EXPECTED = build_web.test_file_bytes()
EXPECTED_SHA = hashlib.sha256(EXPECTED).hexdigest()
TITLE = b"Franken-E58 M5"


def request(host, method, path, timeout):
    """One request on a fresh connection. Returns (status, headers, raw body, seconds)."""
    t0 = time.perf_counter()
    conn = http.client.HTTPConnection(host, 80, timeout=timeout)
    try:
        conn.putrequest(method, path, skip_accept_encoding=True)  # path sent exactly as given
        conn.putheader("Accept-Encoding", "gzip")
        if method != "GET":
            conn.putheader("Content-Length", "0")
        conn.endheaders()
        resp = conn.getresponse()
        body = resp.read()
        return resp.status, dict((k.lower(), v) for k, v in resp.getheaders()), body, time.perf_counter() - t0
    finally:
        conn.close()


def decode(headers, body):
    return gzip.decompress(body) if headers.get("content-encoding") == "gzip" else body


def ms(x):
    return f"{x * 1000:.1f}"


def functional(host, timeout):
    out = []
    # GET / x 200
    lat, ok, fail = [], 0, 0
    for _ in range(200):
        try:
            st, h, b, dt = request(host, "GET", "/", timeout)
            if st == 200 and TITLE in decode(h, b) and h.get("content-encoding") == "gzip":
                ok += 1
                lat.append(dt)
            else:
                fail += 1
        except OSError:
            fail += 1
    if lat:
        out.append(f"GET /           : {ok}/200 OK, latency min/avg/max {ms(min(lat))}/{ms(statistics.mean(lat))}/"
                   f"{ms(max(lat))} ms")
    else:
        out.append(f"GET /           : 0/200 OK")
    # GET /test.bin x 20
    good, times, wire = 0, [], 0
    for _ in range(20):
        try:
            st, h, b, dt = request(host, "GET", "/test.bin", timeout)
            data = decode(h, b)
            if st == 200 and len(data) == len(EXPECTED) and hashlib.sha256(data).hexdigest() == EXPECTED_SHA:
                good += 1
                times.append(dt)
                wire += len(b)
        except OSError:
            pass
    thr = (wire / sum(times) / 1024) if times else 0.0
    out.append(f"GET /test.bin   : {good}/20 match length {len(EXPECTED)} B and sha256 {EXPECTED_SHA[:16]}..., "
               f"throughput {thr:.1f} KiB/s on the wire"
               + (f", time min/avg/max {ms(min(times))}/{ms(statistics.mean(times))}/{ms(max(times))} ms" if times else ""))
    # Negative cases and /version
    cases = [("GET", "/no-such-file", 404), ("POST", "/", 405), ("PUT", "/index.html", 405),
             ("GET", "/../index.html", 400), ("GET", "/a/../index.html", 400), ("GET", "/ws", 404),
             ("GET", "/version", 200)]
    cases_ok = True
    for method, path, want in cases:
        try:
            st, h, b, _ = request(host, method, path, timeout)
        except OSError as e:
            st, b = f"error {e}", b""
        extra = f" {b.decode(errors='replace')}" if path == "/version" and st == 200 else ""
        cases_ok = cases_ok and st == want
        out.append(f"{method:4} {path:17}: {st} (want {want}) {'OK' if st == want else 'FAIL'}{extra}")
    passed = ok == 200 and good == 20 and cases_ok
    return out, passed


def load(host, seconds, timeout):
    pages = files = errors = 0
    lat = []
    t_end = time.monotonic() + seconds
    next_page = next_file = time.monotonic()
    while time.monotonic() < t_end:
        now = time.monotonic()
        try:
            if now >= next_file:
                st, h, b, dt = request(host, "GET", "/test.bin", timeout)
                files += 1
                errors += st != 200
                next_file += 2.0
            if now >= next_page:
                st, h, b, dt = request(host, "GET", "/", timeout)
                pages += 1
                errors += st != 200
                lat.append(dt)
                next_page += 0.05
        except OSError:
            errors += 1
        time.sleep(max(0.0, min(next_page, next_file) - time.monotonic()))
    line = (f"--load {seconds} s: {pages} page and {files} test-file requests, {errors} errors"
            + (f", page latency min/avg/max {ms(min(lat))}/{ms(statistics.mean(lat))}/{ms(max(lat))} ms" if lat else ""))
    return [line], errors == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--load", type=int, metavar="SECONDS", help="load mode for M5-IMU-2")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()
    lines, ok = load(args.host, args.load, args.timeout) if args.load else functional(args.host, args.timeout)
    print("===== M5 HTTP client summary =====")
    print(f"host {args.host}")
    for line in lines:
        print(line)
    print(f"RESULT: {'PASS' if ok else 'FAIL'}")
    print("===== end =====")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
