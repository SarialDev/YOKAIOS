#!/usr/bin/env python3
"""Peer pe gazda pentru test=lwip lwipmode=host-ping: QEMU ruleaza cu `-netdev socket,listen=127.0.0.1:<port>`, iar
scriptul se conecteaza (cadre Ethernet cu prefix de lungime pe 4 octeti, big endian), rezolva ARP pentru oaspete si
trimite 3 ping-uri (ultimul cu 1472 octeti, cadru de 1514). Uz: lwip_peer.py <port>. Iese cu 0 daca toate au raspuns."""
import socket
import struct
import sys
import time

ME_MAC = bytes.fromhex("525400123499")
ME_IP = bytes([10, 0, 2, 2])
GUEST_IP = bytes([10, 0, 2, 15])


def csum(b):
    if len(b) & 1:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


class Link:
    def __init__(self, port):
        deadline = time.time() + 30
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.3)
        self.buf = b""

    def send(self, frame):
        self.s.sendall(struct.pack("!I", len(frame)) + frame)

    def recv(self, timeout):
        end = time.time() + timeout
        while True:
            if len(self.buf) >= 4:
                n = struct.unpack("!I", self.buf[:4])[0]
                if len(self.buf) >= 4 + n:
                    f = self.buf[4:4 + n]
                    self.buf = self.buf[4 + n:]
                    return f
            left = end - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return None
            if not d:
                return None
            self.buf += d


def arp(op, smac, sip, tmac, tip):
    return struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, op, smac, sip, tmac, tip)


def eth(dst, src, typ, payload):
    return dst + src + struct.pack("!H", typ) + payload


def answer_arp(link, f):
    # raspunde cererilor ARP ale oaspetelui pentru 10.0.2.2
    if len(f) >= 42 and f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x01" and f[38:42] == ME_IP:
        link.send(eth(f[6:12], ME_MAC, 0x0806, arp(2, ME_MAC, ME_IP, f[22:28], f[28:32])))


def main(port):
    link = Link(port)
    guest_mac = None
    deadline = time.time() + 20
    while guest_mac is None and time.time() < deadline:
        link.send(eth(b"\xff" * 6, ME_MAC, 0x0806, arp(1, ME_MAC, ME_IP, b"\0" * 6, GUEST_IP)))
        end = time.time() + 1.5
        while time.time() < end:
            f = link.recv(end - time.time())
            if f is None:
                break
            answer_arp(link, f)
            if len(f) >= 42 and f[12:14] == b"\x08\x06" and f[20:22] == b"\x00\x02" and f[28:32] == GUEST_IP:
                guest_mac = f[22:28]
                break
    if guest_mac is None:
        print("peer: fara raspuns ARP de la 10.0.2.15")
        return 1
    print("peer: ARP ok, oaspete %s" % guest_mac.hex(":"))
    ident = 0x7001
    for seq, plen in ((1, 32), (2, 56), (3, 1472)):
        data = bytes((i * 3 + seq) & 255 for i in range(plen))
        icmp = struct.pack("!BBHHH", 8, 0, 0, ident, seq) + data
        icmp = icmp[:2] + struct.pack("!H", csum(icmp)) + icmp[4:]
        ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(icmp), 0x1000 + seq, 0x4000, 64, 1, 0, ME_IP, GUEST_IP)
        ip = ip[:10] + struct.pack("!H", csum(ip)) + ip[12:]
        link.send(eth(guest_mac, ME_MAC, 0x0800, ip + icmp))
        end = time.time() + 5
        ok = False
        while time.time() < end and not ok:
            f = link.recv(end - time.time())
            if f is None:
                break
            answer_arp(link, f)
            if len(f) < 14 + 20 + 8 or f[12:14] != b"\x08\x00" or f[14 + 9] != 1:
                continue
            ihl = (f[14] & 15) * 4
            r = f[14 + ihl:]
            if csum(f[14:14 + ihl]) != 0 or csum(r) != 0:
                print("peer: checksum gresit in raspuns")
                return 1
            if r[0] == 0 and struct.unpack("!HH", r[4:8]) == (ident, seq) and r[8:] == data and \
               f[14 + 12:14 + 16] == GUEST_IP:
                ok = True
        if not ok:
            print("peer: fara echo reply pentru seq %d" % seq)
            return 1
        print("peer: echo reply seq %d (%d octeti) ok" % (seq, plen))
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main(int(sys.argv[1])))
