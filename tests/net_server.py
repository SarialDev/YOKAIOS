#!/usr/bin/env python3
"""Partea de pe gazda a test=net (T9.T). Uz: net_server.py <port-hostfwd> <port-http>.
Porneste pe 127.0.0.1:<port-http> un server HTTP (oaspetele il vede ca 10.0.2.2:<port-http>) cu /file.bin (300000 octeti
pseudo-aleatori, seed fix) si /file.sha256 (SHA-256-ul lui in hex); apoi se conecteaza prin hostfwd la portul 7 al oaspetelui,
trimite 200000 octeti, inchide partea de scriere si verifica ca primeste exact aceiasi octeti inapoi (echo TCP).
Iese cu 0 daca oaspetele a descarcat ambele fisiere si echo-ul e corect."""
import hashlib
import http.server
import random
import socket
import socketserver
import sys
import threading
import time

rnd = random.Random(1234)
FILE = rnd.getrandbits(8 * 300000).to_bytes(300000, "little")
SHA = hashlib.sha256(FILE).hexdigest()
ECHO = rnd.getrandbits(8 * 200000).to_bytes(200000, "little")
served = set()
errors = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path == "/file.bin":
            body = FILE
        elif self.path == "/file.sha256":
            body = (SHA + "  file.bin\n").encode()
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
        served.add(self.path)


class HTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def echo_through_hostfwd(hp):
    deadline = time.time() + 90
    while time.time() < deadline:
        s = None
        try:
            s = socket.create_connection(("127.0.0.1", hp), timeout=10)
            # hostfwd accepta conexiunea chiar daca oaspetele nu asculta inca: daca nu raspunde, reincercam
            s.settimeout(120)
            s.sendall(ECHO)
            s.shutdown(socket.SHUT_WR)
            data = b""
            while True:
                d = s.recv(65536)
                if not d:
                    break
                data += d
            s.close()
            if data == ECHO:
                print("host: echo TCP prin hostfwd corect (%d octeti)" % len(data))
                return True
            errors.append("echo gresit: primit %d octeti din %d" % (len(data), len(ECHO)))
            return False
        except OSError as e:
            if s:
                s.close()
            if time.time() > deadline - 5:
                errors.append("echo: %s" % e)
            time.sleep(0.5)
    errors.append("nu ma pot conecta la portul 7 al oaspetelui (hostfwd)")
    return False


if __name__ == "__main__":
    hp, http_port = int(sys.argv[1]), int(sys.argv[2])
    srv = HTTP(("127.0.0.1", http_port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("host: HTTP pe 127.0.0.1:%d, sha256 %s" % (http_port, SHA))
    ok = echo_through_hostfwd(hp)
    for _ in range(100):
        if served == {"/file.bin", "/file.sha256"}:
            break
        time.sleep(0.1)
    if served != {"/file.bin", "/file.sha256"}:
        errors.append("oaspetele n-a descarcat tot: %s" % sorted(served))
    for e in errors:
        print("host: " + e)
    ok = ok and not errors
    print("host: %s" % ("ok" if ok else "ESEC"))
    sys.exit(0 if ok else 1)
