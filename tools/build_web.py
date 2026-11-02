#!/usr/bin/env python3
"""Build the LittleFS content (M5): gzip every file in web/ into data/<name>.gz and generate
the deterministic test file data/test.bin.gz. Run before `pio run -t buildfs`.

The test file is TEST_FILE_BYTES bytes from a fixed-seed xorshift32 generator, so it does not
compress and tools/m5_http_test.py can regenerate it to check the SHA-256.
"""
import gzip
import hashlib
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
WEB = ROOT / "web"
DATA = ROOT / "data"
TEST_FILE_NAME = "test.bin"
TEST_FILE_BYTES = 100 * 1024
TEST_FILE_SEED = 0x00E58E58


def test_file_bytes(n=TEST_FILE_BYTES, seed=TEST_FILE_SEED):
    """Deterministic, incompressible content: xorshift32, little-endian words."""
    x = seed
    out = bytearray()
    while len(out) < n:
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        out += x.to_bytes(4, "little")
    return bytes(out[:n])


def gz(data):
    return gzip.compress(data, compresslevel=9, mtime=0)  # mtime 0: same bytes every build


def main():
    DATA.mkdir(exist_ok=True)
    for old in DATA.glob("*.gz"):
        old.unlink()
    total = 0
    for src in sorted(p for p in WEB.rglob("*") if p.is_file()):
        rel = src.relative_to(WEB)
        dst = DATA / (str(rel) + ".gz")
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(gz(src.read_bytes()))
        total += dst.stat().st_size
        print(f"{rel}: {src.stat().st_size} B -> {dst.relative_to(ROOT)} {dst.stat().st_size} B")
    body = test_file_bytes()
    dst = DATA / (TEST_FILE_NAME + ".gz")
    dst.write_bytes(gz(body))
    total += dst.stat().st_size
    print(f"{TEST_FILE_NAME}: {len(body)} B, sha256 {hashlib.sha256(body).hexdigest()} -> "
          f"{dst.relative_to(ROOT)} {dst.stat().st_size} B")
    print(f"total {total} B in data/ (partition 1048576 B)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
