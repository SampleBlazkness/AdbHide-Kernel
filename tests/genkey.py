#!/usr/bin/env python3
"""Generate a throwaway RSA-2048 ADB host key for the in-device tests.

Writes:
  <out>/adb_keys     - device side key store (adb_keys format, one line)
  <out>/hostkey.txt  - n/d in hex for the host tester

This is test tooling only.  It is NOT used by usbgwd and never ships in the
release artifact.
"""
import base64
import os
import secrets
import struct
import sys

SMALL_PRIMES = [3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59,
                61, 67, 71, 73, 79, 83, 89, 97]


def is_probable_prime(n, rounds=32):
    if n < 2:
        return False
    for p in SMALL_PRIMES:
        if n % p == 0:
            return n == p
    d, s = n - 1, 0
    while d % 2 == 0:
        d //= 2
        s += 1
    for _ in range(rounds):
        a = secrets.randbelow(n - 3) + 2
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(s - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def gen_prime(bits):
    while True:
        p = secrets.randbits(bits) | (1 << (bits - 1)) | 1
        if p % 65537 == 0:
            continue
        if is_probable_prime(p):
            return p


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    e = 65537
    p, q = gen_prime(1024), gen_prime(1024)
    n = p * q
    d = pow(e, -1, (p - 1) * (q - 1))

    words = 64
    n_words = [(n >> (32 * i)) & 0xFFFFFFFF for i in range(words)]
    n0inv = (-pow(n, -1, 1 << 32)) % (1 << 32)
    rr = pow(2, 4096, n)
    rr_words = [(rr >> (32 * i)) & 0xFFFFFFFF for i in range(words)]

    blob = struct.pack("<II", words, n0inv)
    blob += b"".join(struct.pack("<I", w) for w in n_words)
    blob += b"".join(struct.pack("<I", w) for w in rr_words)
    blob += struct.pack("<I", e)
    assert len(blob) == 524, len(blob)

    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "adb_keys"), "w") as f:
        f.write(base64.b64encode(blob).decode() + " adbng-selftest@host\n")
    with open(os.path.join(out, "hostkey.txt"), "w") as f:
        f.write("n=%0512x\n" % n)
        f.write("d=%0512x\n" % d)
    print("generated key: n=%d bits, e=%d" % (n.bit_length(), e))


if __name__ == "__main__":
    main()