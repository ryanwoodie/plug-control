#!/usr/bin/env python3
"""Delete the retained messages for every owner in a secrets.h.

make_secrets.py keeps the previous file as secrets/secrets.prev.h; after
reflashing, run `tools/clear_topics.py secrets/secrets.prev.h` so old
encrypted messages don't linger on the broker. Plain MQTT 3.1.1 over TCP, no dependencies.
"""
import os, re, socket, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST, PORT = "broker.hivemq.com", 1883


def packet(kind, body):
    n, rem = len(body), b""
    while True:
        b, n = n % 128, n // 128
        rem += bytes([b | (0x80 if n else 0)])
        if not n:
            return bytes([kind]) + rem + body


def s(x):
    return struct.pack(">H", len(x)) + x.encode()


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "firmware/include/secrets.h")
    tids = re.findall(r'"([0-9a-f]{20})"', open(path).read())
    if not tids:
        sys.exit("no owners found")
    sock = socket.create_connection((HOST, PORT), timeout=10)
    sock.sendall(packet(0x10, s("MQTT") + bytes([4, 2]) + struct.pack(">H", 30) + s("pc-clear-" + os.urandom(3).hex())))
    if sock.recv(4)[3] != 0:
        sys.exit("connect refused")
    for tid in tids:
        for kind in "cxs":
            sock.sendall(packet(0x31, s(f"plugctl/{tid}/{kind}")))  # retained, empty payload = delete
        print("cleared", tid)
    sock.sendall(packet(0xE0, b""))
    sock.close()


if __name__ == "__main__":
    main()
