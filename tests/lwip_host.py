#!/usr/bin/env python3
"""Partea de pe gazda a test=lwip: se conecteaza prin hostfwd la serverele echo ale sistemului (TCP si UDP, portul 7 al
oaspetelui) si verifica ce se intoarce. Uz: lwip_host.py <port-gazda>. Iese cu 0 daca totul e corect."""
import socket
import sys
import threading
import time

TCP_TOTAL = 262144


def pat(i):
    return (i * 131 + (i >> 8) * 7 + 5) & 255


def tcp_test(port):
    deadline = time.time() + 30
    s = None
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            # slirp accepta conexiunea gazdei chiar daca oaspetele nu asculta inca; un RST vine la primul read/write
            s.sendall(bytes(pat(i) for i in range(1)))
            break
        except OSError:
            if s:
                s.close()
                s = None
            time.sleep(0.5)
    if s is None:
        print("host: tcp: nu ma pot conecta")
        return False
    sent = [1]
    err = []

    def sender():
        try:
            i = 1
            while i < TCP_TOTAL:
                n = min(4000, TCP_TOTAL - i)
                s.sendall(bytes(pat(i + k) for k in range(n)))
                i += n
            sent[0] = i
            s.shutdown(socket.SHUT_WR)
        except OSError as e:
            err.append(str(e))

    th = threading.Thread(target=sender)
    th.start()
    got = 0
    s.settimeout(30)
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            for k, b in enumerate(d):
                if b != pat(got + k):
                    print("host: tcp: octet gresit la %d" % (got + k))
                    return False
            got += len(d)
    except OSError as e:
        print("host: tcp: eroare la citire: %s (primit %d)" % (e, got))
        return False
    th.join()
    s.close()
    if err or got != TCP_TOTAL:
        print("host: tcp: primit %d din %d %s" % (got, TCP_TOTAL, err))
        return False
    print("host: tcp echo %d octeti ok" % got)
    return True


def udp_test(port):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(10)
    for k, n in enumerate((9, 1000, 1400)):
        data = bytes(pat(i + k * 17) for i in range(n))
        ok = False
        for _ in range(5):
            u.sendto(data, ("127.0.0.1", port))
            try:
                r, _a = u.recvfrom(4096)
            except socket.timeout:
                continue
            ok = r == data
            break
        if not ok:
            print("host: udp: datagrama %d (%d octeti) nu s-a intors identica" % (k, n))
            return False
    print("host: udp echo ok")
    return True


if __name__ == "__main__":
    p = int(sys.argv[1])
    ok = tcp_test(p) and udp_test(p)
    sys.exit(0 if ok else 1)
