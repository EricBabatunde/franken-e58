#!/usr/bin/env python3
"""Control frame codec for the M6 tools (blueprint section 4.6). Standard library only.

14 bytes, little-endian: magic 0xE5, type (1 control, 2 ping, 3 pong), seq u16, roll i16,
pitch i16, yaw i16, throttle u16, flags u8, crc8 over bytes 0 to 12.
CRC-8: polynomial 0x07, init 0x00, no reflection, no final XOR (CRC-8/SMBUS, check 0xF4).
A pong echoes seq, roll, pitch, yaw and flags; throttle carries the device turnaround in us.

This is a table-driven CRC; the firmware's (components/comms/control_frame.cpp) is bitwise.
Both are checked against the shared known-answer vectors. Run this file to check them:
    python3 tools/control_frame.py
"""
import pathlib
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
VECTORS = ROOT / "components" / "comms" / "test_vectors" / "control_frame_vectors.txt"

FRAME_BYTES = 14
MAGIC = 0xE5
CONTROL, PING, PONG = 1, 2, 3
FLAG_ARM, FLAG_KILL, FLAG_ANGLE = 1, 2, 4
ERRORS = ("ok", "length", "magic", "crc", "type")
_STRUCT = struct.Struct("<BBHhhhHB")  # bytes 0 to 12


def _make_table():
    table = []
    for byte in range(256):
        crc = byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
        table.append(crc)
    return table


_TABLE = _make_table()


def crc8(data):
    crc = 0
    for b in data:
        crc = _TABLE[crc ^ b]
    return crc


def encode(ftype, seq, roll=0, pitch=0, yaw=0, throttle=0, flags=0):
    body = _STRUCT.pack(MAGIC, ftype, seq & 0xFFFF, roll, pitch, yaw, throttle, flags)
    return body + bytes([crc8(body)])


def decode(buf):
    """Returns (error, fields). error is one of ERRORS; fields is a dict when error is 'ok'."""
    if len(buf) != FRAME_BYTES:
        return "length", None
    if buf[0] != MAGIC:
        return "magic", None
    if crc8(buf[:13]) != buf[13]:
        return "crc", None
    if not CONTROL <= buf[1] <= PONG:
        return "type", None
    _, ftype, seq, roll, pitch, yaw, throttle, flags = _STRUCT.unpack(buf[:13])
    return "ok", dict(type=ftype, seq=seq, roll=roll, pitch=pitch, yaw=yaw, throttle=throttle, flags=flags)


def make_pong(ping_fields, turnaround_us):
    f = dict(ping_fields)
    return encode(PONG, f["seq"], f["roll"], f["pitch"], f["yaw"], min(int(turnaround_us), 0xFFFF), f["flags"])


def _hex(s):
    return b"" if s == "-" else bytes.fromhex(s)


def run_vectors(path=VECTORS):
    """Checks this codec against the shared vectors. Returns (checked, failures list)."""
    checked, failures = 0, []
    for n, raw in enumerate(pathlib.Path(path).read_text().splitlines(), 1):
        tok = raw.split("#", 1)[0].split()
        if not tok:
            continue
        kind = tok[0]
        try:
            if kind == "crc":
                checked += 1
                ok = len(tok) == 3 and crc8(_hex(tok[1])) == int(tok[2], 0)
            elif kind == "frame":
                checked += 1
                err, fields = decode(_hex(tok[1]))
                ok = err == tok[2]
                if ok and err == "ok":
                    checked += 2
                    want = [int(x, 0) for x in tok[3:10]]
                    got = [fields[k] for k in ("type", "seq", "roll", "pitch", "yaw", "throttle", "flags")]
                    ok = len(tok) == 10 and got == want and encode(*got[:2], *got[2:]) == _hex(tok[1])
                else:
                    ok = ok and len(tok) == 3
            elif kind == "pong":
                checked += 1
                err, fields = decode(_hex(tok[1]))
                ok = len(tok) == 4 and err == "ok" and make_pong(fields, int(tok[2], 0)) == _hex(tok[3])
            else:
                ok = False
        except (ValueError, IndexError):
            ok = False
        if not ok:
            failures.append(n)
    return checked, failures


def main():
    checked, failures = run_vectors()
    print(f"control frame vectors: {checked} checks, {len(failures)} failed"
          + (f" (lines {failures})" if failures else "") + f" -> {'PASS' if not failures else 'FAIL'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
