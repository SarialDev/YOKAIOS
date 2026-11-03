#!/usr/bin/env python3
"""Partea de pe gazda a test=netutils (T9.5). Uz: netutils_host.py <port-hostfwd> <port-http> <port-echo>.
Porneste (pe 127.0.0.1, la care oaspetele ajunge ca 10.0.2.2):
  - un server HTTP/1.0 pe <port-http>: /hello.txt, /  (index), /big.bin (150000 octeti), /redir (302 relativ), /redir2 (302
    absolut), /loop (redirectare la sine), /empty (200 fara corp), /short (Content-Length mai mare decat corpul), /missing (404);
    verifica Host: 10.0.2.2:<port> si User-Agent: Yokai-wget;
  - un server echo cu litere mari pe <port-echo>, TCP (citeste pana la EOF, raspunde, inchide) si UDP; cand primeste
    NETUTILS-DONE termina scriptul.
Apoi se conecteaza prin hostfwd la `nc -l 7` din oaspete, trimite "ping from host\\n" si asteapta "pong from guest\\n".
Iese cu 0 daca totul e corect si oaspetele a trimis NETUTILS-DONE."""
import http.server
import socket
import socketserver
import sys
import threading
import time

BIG = 150000
done = threading.Event()
errors = []


def pat(i):
    return (i * 131 + (i >> 8) * 7) & 255


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "yokai-test"

    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        want = "10.0.2.2:%d" % self.server.server_address[1]
        if self.headers.get("Host") != want or not self.headers.get("User-Agent", "").startswith("Yokai-wget"):
            errors.append("antete gresite: Host=%r UA=%r" % (self.headers.get("Host"), self.headers.get("User-Agent")))
            self.send_error(400)
            return
        p = self.path
        if p == "/hello.txt":
            self.reply(b"hello from host\n")
        elif p == "/":
            self.reply(b"index page\n")
        elif p == "/big.bin":
            self.reply(bytes(pat(i) for i in range(BIG)))
        elif p == "/redir":
            self.redirect("/hello.txt")
        elif p == "/redir2":
            self.redirect("http://10.0.2.2:%d/hello.txt" % self.server.server_address[1])
        elif p == "/loop":
            self.redirect("/loop")
        elif p == "/empty":
            self.reply(b"")
        elif p == "/short":
            self.send_response(200)
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"only ten b")
        else:
            self.send_error(404)

    def redirect(self, loc):
        self.send_response(302)
        self.send_header("Location", loc)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def reply(self, body):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class HTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def tcp_echo(port):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(5)

    def one(c):
        c.settimeout(30)
        data = b""
        try:
            while True:
                d = c.recv(65536)
                if not d:
                    break
                data += d
            c.sendall(data.upper())
            if data == b"NETUTILS-DONE\n":
                done.set()
        except OSError:
            pass
        finally:
            c.close()

    while True:
        c, _ = ls.accept()
        threading.Thread(target=one, args=(c,), daemon=True).start()


def udp_echo(port):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", port))
    while True:
        d, a = u.recvfrom(65536)
        u.sendto(d.upper(), a)


def talk_to_guest(hp):
    deadline = time.time() + 60
    while time.time() < deadline:
        s = None
        try:
            s = socket.create_connection(("127.0.0.1", hp), timeout=10)
            s.sendall(b"ping from host\n")
            s.shutdown(socket.SHUT_WR)
            s.settimeout(30)
            data = b""
            while True:
                d = s.recv(100)
                if not d:
                    break
                data += d
            s.close()
            if data == b"pong from guest\n":
                print("host: nc -l din oaspete a raspuns corect")
                return True
            errors.append("raspuns gresit de la nc -l: %r" % data)
            return False
        except OSError:
            if s:
                s.close()
            time.sleep(0.5)
    errors.append("nu ma pot conecta la nc -l (hostfwd)")
    return False


if __name__ == "__main__":
    hp, http_port, echo_port = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    srv = HTTP(("127.0.0.1", http_port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    threading.Thread(target=tcp_echo, args=(echo_port,), daemon=True).start()
    threading.Thread(target=udp_echo, args=(echo_port,), daemon=True).start()
    ok = talk_to_guest(hp)
    if not done.wait(120):
        errors.append("oaspetele n-a trimis NETUTILS-DONE")
        ok = False
    for e in errors:
        print("host: " + e)
    print("host: %s" % ("ok" if ok and not errors else "ESEC"))
    sys.exit(0 if ok and not errors else 1)
