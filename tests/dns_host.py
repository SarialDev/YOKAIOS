#!/usr/bin/env python3
"""Server DNS de test pentru test=dhcp (T9.4). Uz: dns_host.py <port>. Asculta UDP pe 127.0.0.1:<port> (oaspetele ajunge
la el ca 10.0.2.2#<port>). yokai.test -> 10.9.8.7; www.yokai.test -> CNAME yokai.test (cu pointer de comprimare) + A 10.9.8.8;
orice altceva -> NXDOMAIN. Iese singur dupa 180 s."""
import socket
import struct
import sys
import time


def parse_name(m, o):
    labels = []
    while m[o]:
        n = m[o]
        labels.append(m[o + 1:o + 1 + n].decode())
        o += 1 + n
    return ".".join(labels).lower(), o + 1


def answer(q):
    qid, flags, qd = struct.unpack(">HHH", q[:6])
    name, o = parse_name(q, 12)
    qtype, qclass = struct.unpack(">HH", q[o:o + 4])
    question = q[12:o + 4]
    print("host: dns: %s tipul %d" % (name, qtype), flush=True)
    if qtype != 1:
        return struct.pack(">HHHHHH", qid, 0x8180, 1, 0, 0, 0) + question
    if name == "yokai.test":
        rr = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + bytes([10, 9, 8, 7])
        return struct.pack(">HHHHHH", qid, 0x8180, 1, 1, 0, 0) + question + rr
    if name == "www.yokai.test":
        target = b"\x05yokai\x04test\x00"
        cname = b"\xc0\x0c" + struct.pack(">HHIH", 5, 1, 60, len(target)) + target
        a = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + bytes([10, 9, 8, 8])
        return struct.pack(">HHHHHH", qid, 0x8180, 1, 2, 0, 0) + question + cname + a
    return struct.pack(">HHHHHH", qid, 0x8183, 1, 0, 0, 0) + question


def main():
    port = int(sys.argv[1])
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", port))
    s.settimeout(1)
    end = time.time() + 180
    while time.time() < end:
        try:
            q, addr = s.recvfrom(512)
        except socket.timeout:
            continue
        try:
            s.sendto(answer(q), addr)
        except (IndexError, struct.error, UnicodeDecodeError):
            pass


main()
