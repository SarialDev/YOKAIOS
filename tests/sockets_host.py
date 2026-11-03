#!/usr/bin/env python3
"""Partea de pe gazda a test=sockets (T9.3). Uz: sockets_host.py <port-hostfwd> <port-server-gazda>.
1. Porneste imediat un server pe 127.0.0.1:<port-server-gazda> (oaspetele ii spune 10.0.2.2:<port>): citeste "hello yokai\\n",
   raspunde cu litere mari, asteapta EOF (shutdown(WR) din oaspete), trimite "bye\\n" si inchide.
2. Doi clienti TCP simultani spre serverul echo al oaspetelui (hostfwd -> portul 7), fiecare 100000 de octeti, apoi 3
   datagrame UDP. Iese cu 0 daca totul e corect."""
import socket
import sys
import threading
import time

TOTAL = 100000


def pat(i, salt):
    return (i * 131 + (i >> 8) * 7 + salt) & 255


def server(port, result):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(2)
    ls.settimeout(120)
    try:
        c, _ = ls.accept()
        c.settimeout(60)
        data = b""
        while not data.endswith(b"\n"):
            d = c.recv(100)
            if not d:
                raise OSError("oaspetele a inchis prea devreme")
            data += d
        if data != b"hello yokai\n":
            raise OSError("mesaj gresit: %r" % data)
        c.sendall(data.upper())
        while c.recv(100):
            pass                                    # pana la EOF (shutdown(WR) din oaspete)
        c.sendall(b"bye\n")
        c.close()
        print("host: server: sesiune cu oaspetele ok")
        result.append(True)
    except OSError as e:
        print("host: server: %s" % e)
        result.append(False)
    finally:
        ls.close()


def connect(port):
    deadline = time.time() + 40
    while time.time() < deadline:
        s = None
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=10)
            # slirp accepta conexiunea gazdei chiar daca oaspetele nu asculta inca; un RST vine la prima citire/scriere
            s.sendall(b"")
            return s
        except OSError:
            if s:
                s.close()
            time.sleep(0.5)
    return None


def echo_client(port, salt, results, idx):
    s = connect(port)
    if s is None:
        print("host: client %d: nu ma pot conecta" % idx)
        results[idx] = False
        return
    err = []

    def sender():
        try:
            i = 0
            while i < TOTAL:
                n = min(3000, TOTAL - i)
                s.sendall(bytes(pat(i + k, salt) for k in range(n)))
                i += n
            s.shutdown(socket.SHUT_WR)
        except OSError as e:
            err.append(str(e))

    th = threading.Thread(target=sender)
    th.start()
    got = 0
    s.settimeout(60)
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            for k, b in enumerate(d):
                if b != pat(got + k, salt):
                    print("host: client %d: octet gresit la %d" % (idx, got + k))
                    results[idx] = False
                    return
            got += len(d)
    except OSError as e:
        print("host: client %d: eroare la citire: %s (primit %d)" % (idx, e, got))
        results[idx] = False
        return
    th.join()
    s.close()
    ok = not err and got == TOTAL
    if not ok:
        print("host: client %d: primit %d din %d %s" % (idx, got, TOTAL, err))
    else:
        print("host: client %d: echo %d octeti ok" % (idx, got))
    results[idx] = ok


def udp_test(port):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.settimeout(10)
    for k, n in enumerate((9, 1000, 1400)):
        data = bytes(pat(i, 40 + k) for i in range(n))
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
    hp, sp = int(sys.argv[1]), int(sys.argv[2])
    srv = []
    th = threading.Thread(target=server, args=(sp, srv))
    th.start()
    res = [None, None]
    cl = [threading.Thread(target=echo_client, args=(hp, 3 + i, res, i)) for i in range(2)]
    for t in cl:
        t.start()
    for t in cl:
        t.join()
    ok = all(res) and udp_test(hp)
    th.join()
    ok = ok and bool(srv) and srv[0]
    sys.exit(0 if ok else 1)
